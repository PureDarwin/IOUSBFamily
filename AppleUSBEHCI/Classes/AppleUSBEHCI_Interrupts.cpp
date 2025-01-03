/*
 * Copyright � 1998-2012 Apple Inc.  All rights reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <libkern/OSByteOrder.h>


#include <IOKit/usb/IOUSBLog.h>
#include <IOKit/usb/IOUSBHubPolicyMaker.h>
#include <IOKit/usb/IOUSBRootHubDevice.h>
#include "AppleUSBEHCI.h"
#include "USBTracepoints.h"

#define super IOUSBControllerV3
#define self this

// Delete this later and clean up
//#define EndianSwap32Bit USBToHostLong


#if EHCI_USE_KPRINTF
#undef USBLog
#undef USBError
void kprintf(const char *format, ...)
__attribute__((format(printf, 1, 2)));
#define USBLog( LEVEL, FORMAT, ARGS... )  if ((LEVEL) <= EHCI_USE_KPRINTF) { kprintf( FORMAT "\n", ## ARGS ) ; }
#define USBError( LEVEL, FORMAT, ARGS... )  { kprintf( FORMAT "\n", ## ARGS ) ; }
#endif


void 
AppleUSBEHCI::PollInterrupts(IOUSBCompletionAction safeAction)
{
	USBTrace_Start( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts, (uintptr_t)this, _errorInterrupt, _completeInterrupt, _portChangeInterrupt );

    if (_hostErrorInterrupt & kEHCIHostErrorIntBit)
    {
        _hostErrorInterrupt = 0;
        
		// Host System Error - this is a serious error on the PCI bus
		// Only print it every power of 2 times
		//
		if ( ++_errors.hostSystemError == (UInt32) (1L << _errors.displayed) )
		{
			showRegisters(1, "Host System Error");
			USBLog(1, "AppleUSBEHCI[%p]::PollInterrupts - Host System Error Occurred (fatal PCI error) - not restarted - USBCMD(0x%x) USBSTS(0x%x)",  this, USBToHostLong(_pEHCIRegisters->USBCMD), USBToHostLong(_pEHCIRegisters->USBSTS));
			USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, _errors.displayed, 0, 1 );
			_errors.displayed++;
#if DEBUG_LEVEL != DEBUG_LEVEL_PRODUCTION
			panic("AppleUSBEHCI[%p] - Host System Error! This is a hardware/PCI error!! USBCMD(0x%x) USBSTS(0x%x)\n", this, USBToHostLong(_pEHCIRegisters->USBCMD), USBToHostLong(_pEHCIRegisters->USBSTS));
#endif
		}
    }
	
    if (_errorInterrupt & kEHCIErrorIntBit)
    {
        _errorInterrupt = 0;
        
        USBLog(7, "AppleUSBEHCI[%p]::PollInterrupts - completion (_errorInterrupt) interrupt",  this);
 		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, 0, 0, 4 );
       scavengeCompletedTransactions(safeAction);
    }
	
    if (_completeInterrupt & kEHCICompleteIntBit)
    {
        _completeInterrupt = 0;
        USBLog(7, "AppleUSBEHCI[%p]::PollInterrupts - completion (_completeInterrupt) interrupt",  this);
		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, 0, 0, 3 );
        scavengeCompletedTransactions(safeAction);
    }
	
	 //  Port Change Interrupt
    if (_portChangeInterrupt & kEHCIPortChangeIntBit)
    {
		_portChangeInterrupt = 0;
		
		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, 0, 0, 5 );

        USBLog(6,"AppleUSBEHCI[%p]::PollInterrupts -  Port Change Interrupt on bus 0x%x - ensuring usability", this, (uint32_t)_busNumber );
		EnsureUsability();

		if (_myPowerState == kUSBPowerStateOn)
		{
			// Check to see if we are resuming the port
			RHCheckForPortResumes();
		}
		else
		{
			USBLog(2, "AppleUSBEHCI[%p]::PollInterrupts - deferring checking for RHStatus until we are running again", this);
		}
    }

	//Async Advance Interrupt
    if (_asyncAdvanceInterrupt & kEHCIAAEIntBit)
    {
        _asyncAdvanceInterrupt = 0;
		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, 0, 0, 6 );
		USBLog(6, "AppleUSBEHCI[%p]::PollInterrupts - async advance interrupt",  this);
    }
	
	// Frame Rollover Interrupt
    if (_frameRolloverInterrupt & kEHCIFrListRolloverIntBit)
    {
		
        _frameRolloverInterrupt = 0;
		// copy the temporary variables over to the real thing
		// we do this because this method is protected by the workloop gate whereas the FilterInterrupt one is not
		_anchorTime = _tempAnchorTime;
		_anchorFrame = _tempAnchorFrame;
		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts , (uintptr_t)this, 0, 0, 7 );
    
    }

	USBTrace_End( kUSBTEHCIInterrupts, kTPEHCIInterruptsPollInterrupts, (uintptr_t)this, 0, 0, 0 );
}



void
AppleUSBEHCI::InterruptHandler(OSObject *owner, IOInterruptEventSource * /*source*/, int /*count*/)
{
    register 	AppleUSBEHCI		*controller = (AppleUSBEHCI *) owner;
    static 	Boolean 		emitted;
	
    if (!controller || controller->isInactive() || !controller->_controllerAvailable)
	{
#if EHCI_USE_KPRINTF
		kprintf("AppleUSBEHCI::InterruptHandler - Ignoring interrupt\n");
#endif
        return;
	}
	
    if (!emitted)
    {
        emitted = true;
        // USBLog("EHCIUIM -- InterruptHandler Unimplimented not finishPending\n");
    }
	
    controller->PollInterrupts();
}



// This routine will get called at Primary interrupt time. 
//
// At primary interrupt time we are mainly concerned with updating the frStatus and frActCount fields of the frames
// in low latency isoch TD's. We also updated the master frame counter.
//
// Since this is a static method, the work is actually done by the FilterInterrupt, which is an instance method
//
bool 
AppleUSBEHCI::PrimaryInterruptFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
#pragma unused (source)
    register AppleUSBEHCI	*controller = (AppleUSBEHCI *)owner;
    bool					result = true;
	
//	USBTrace_Start( kUSBTEHCIInterrupts, kTPEHCIInterruptsPrimaryInterruptFilter, (uintptr_t)controller );
	
    // If we our controller has gone away, or it's going away, or if we're on a PC Card and we have been ejected,
    // then don't process this interrupt.
    //
    if (!controller || controller->isInactive() || !controller->_controllerAvailable)
	{
#if EHCI_USE_KPRINTF
		kprintf("AppleUSBEHCI[%p]::PrimaryInterruptFilter - Ignoring interrupt\n", controller);
#endif
        return false;
	}

    // Process this interrupt
    //
    controller->_filterInterruptActive = true;
    result = controller->FilterInterrupt(0);
    controller->_filterInterruptActive = false;
	
//	USBTrace_End( kUSBTEHCIInterrupts, kTPEHCIInterruptsPrimaryInterruptFilter, (uintptr_t)controller, result);
	
    return result;
}



bool 
AppleUSBEHCI::FilterInterrupt(int index)
{
#pragma unused (index)
    register UInt32			activeInterrupts;
    register UInt32			enabledInterrupts;
	UInt32					statusClearBits = 0;
    Boolean					needSignal = false;
    uint64_t				timeStamp;
    
    enabledInterrupts = USBToHostLong(_pEHCIRegisters->USBIntr);

	if (enabledInterrupts == kEHCIInvalidRegisterValue)
	{
		// our controller appears to have gone away - perhaps we could put an iVar here to track that and return before reading
		_controllerAvailable = false;
		return false;
	}
	
    activeInterrupts = USBToHostLong(_pEHCIRegisters->USBSTS);

	if (activeInterrupts == kEHCIInvalidRegisterValue)
	{
		// our controller appears to have gone away
		_controllerAvailable = false;
		return false;
	}
	
    activeInterrupts = activeInterrupts & enabledInterrupts;

    if (activeInterrupts != 0)
    {		
		USBTrace( kUSBTEHCIInterrupts, kTPEHCIInterruptsPrimaryInterruptFilter, (uintptr_t)this, enabledInterrupts, activeInterrupts, 0 );

		// One of our 6 interrupts fired.  Process the ones which need to be processed at primary int time
        //
		
        // Frame Number Rollover
        //
        if (activeInterrupts & kEHCIFrListRolloverIntBit)
        {
			uint64_t		tempTime;
			UInt32			frindex;
			
			// get the frame index (if possible) so that we can stamp our Tracepoint but will also bail if it has gone away
			frindex = USBToHostLong(_pEHCIRegisters->FRIndex);
			if (frindex == kEHCIInvalidRegisterValue)
			{
				// we got disconnected
				_controllerAvailable = false;
				return false;
			}
			
			// NOTE: This code depends upon the fact that we do not change the Frame List Size
			// in the USBCMD register. If the frame list size changes, then this code needs to change
			if (frindex < kEHCIFRIndexRolloverBit)
				_frameNumber += kEHCIFrameNumberIncrement;

			_tempAnchorFrame = _frameNumber + (frindex >> 3);
			tempTime = mach_absolute_time();
			_tempAnchorTime = *(AbsoluteTime*)&tempTime;
			_frameRolloverInterrupt = kEHCIFrListRolloverIntBit;
		
			statusClearBits |= kEHCIFrListRolloverIntBit;
        }
		// at the moment, let the secondary interrupt handler get these by signaling
        if (activeInterrupts & kEHCIAAEIntBit)
		{
			_asyncAdvanceInterrupt = kEHCIAAEIntBit;
			statusClearBits |= kEHCIAAEIntBit;
			needSignal = true;
		}
        if (activeInterrupts & kEHCIHostErrorIntBit)
		{
			_hostErrorInterrupt = kEHCIHostErrorIntBit;
			statusClearBits |= kEHCIHostErrorIntBit;
			needSignal = true;
		}
        if (activeInterrupts & kEHCIPortChangeIntBit)
		{
			_portChangeInterrupt = kEHCIPortChangeIntBit;
			statusClearBits |= kEHCIPortChangeIntBit;
			needSignal = true;
		}
        if (activeInterrupts & kEHCIErrorIntBit)
		{
			_errorInterrupt = kEHCIErrorIntBit;
			statusClearBits |= kEHCIErrorIntBit;
			needSignal = true;
		}
        if (activeInterrupts & kEHCICompleteIntBit)
		{
            // Now that we have the beginning of the queue, walk it looking for low latency isoch TD's
            // Use this time as the time stamp time for all the TD's that we processed.  
            //
			timeStamp = mach_absolute_time();

			statusClearBits |= kEHCICompleteIntBit;
			needSignal = true;
			
			// 9385815 - it appears that on some PCIe buses we need to read from a register over the bus AFTER we clear the bits
			// since we are about to read the USBCMD register in the if statement, we go ahead and write to the STS register
			// before we do that
			if (!_inAbortIsochEP)
			{
				_pEHCIRegisters->USBSTS = HostToUSBLong(statusClearBits);							// clear the bit again
				IOSync();
				statusClearBits = 0;
			}
			
			// we need to check the periodic list to see if there are any Isoch TDs which need to come off
			// and potentially have their frame lists updated (for Low Latency) we will place them in reverse
			// order on a "done queue" which will be looked at by the isoch scavanger
			// only do this if the periodic schedule is enabled
			
			// ***** WARNING WARNING WARNING *****
			// Do not switch terms 2 and 3 of this if statement, or you may reintroduce 9385815
			if (!_inAbortIsochEP && (_pEHCIRegisters->USBCMD & HostToUSBLong(kEHCICMDPeriodicEnable)) && (_outSlot < kEHCIPeriodicListEntries))
			{
				IOUSBControllerIsochListElement *cachedHead;
				UInt32							cachedProducer;
				UInt32							frIndex;
				UInt16							curSlot, testSlot, nextSlot, stopSlot;
				UInt16							curMicroFrame;
				
				frIndex = USBToHostLong(_pEHCIRegisters->FRIndex);
				if (frIndex == kEHCIInvalidRegisterValue)
				{
					// we got disconnected
					_controllerAvailable = false;
					return false;
				}
				
				curSlot = (frIndex >> 3) & (kEHCIPeriodicListEntries-1);
				stopSlot = (curSlot+1) & (kEHCIPeriodicListEntries-1);
				curMicroFrame = frIndex & 7;
				
				cachedHead = (IOUSBControllerIsochListElement*)_savedDoneQueueHead;
				cachedProducer = _producerCount;
				testSlot = _outSlot;

				// kprintf("EHCI::FilterInterrupt - testSlot(%d) stopSlot(%d)\n", (int)testSlot, (int)stopSlot);

				while (testSlot != stopSlot)
				{
					IOUSBControllerListElement				*thing, *prevThing, *nextThing;
					IOUSBControllerIsochListElement			*isochEl;
					AppleEHCISplitIsochTransferDescriptor	*splitTD = NULL;
					AppleEHCIIsochTransferDescriptor		*hsIsocTD = NULL;
					bool									needToRescavenge = false;
					
					nextSlot = (testSlot+1) & (kEHCIPeriodicListEntries-1);
					thing = GetPeriodicListLogicalEntry(testSlot);
					prevThing = NULL;
					while(thing != NULL)
					{
						nextThing = thing->_logicalNext;
						isochEl = OSDynamicCast(IOUSBControllerIsochListElement, thing);

						if (!isochEl)
							break;						// only care about Isoch in this list - if we get here we are at the interrupt TDs

						splitTD = OSDynamicCast(AppleEHCISplitIsochTransferDescriptor, isochEl);
						
						// check to see if all of these conditions are met, if so, we can't retire this TD yes
						// 1 - this is a splitTD
						// 2 - the TD wraps around (useBackPtr == true)
						// 3 - the slot after this one is the curslot
						// 4 - we have not gotten to microframe 2 of the curSlot
						if (splitTD && (((AppleEHCIIsochEndpoint*)(splitTD->_pEndpoint))->useBackPtr) && (nextSlot == curSlot) && (curMicroFrame < 2))
						{
							prevThing = thing;
							thing = nextThing;
							needToRescavenge = true;
							continue;
						}
						
						if (!splitTD)
							hsIsocTD = OSDynamicCast(AppleEHCIIsochTransferDescriptor, isochEl);
						
						if (testSlot == curSlot)
						{
							bool scavengeThisThing = false;
							
							if (splitTD)
							{
								UInt32 statFlags = USBToHostLong(splitTD->GetSharedLogical()->statFlags);
								if (statFlags & kEHCIsiTDStatStatusActive)
								{
									// kprintf("EHCI::FilterInterrupt - splitTD(%p) still active.. curFrame(%d) curMicroFrame(%d) testSlot(%d)\n", isochEl, (int)curFrame, (int)curMicroFrame, (int)testSlot);
									scavengeThisThing = false;
								}
								else
								{
									scavengeThisThing = true;
								}

							}
							else if (hsIsocTD)
							{
								int			uFrame;
								UInt32		*transactionPtr = &hsIsocTD->GetSharedLogical()->Transaction0;

								for (uFrame = 0; uFrame < kEHCIuFramesPerFrame; uFrame++)
								{
									if (USBToHostLong(*transactionPtr++) & kEHCI_ITDStatus_Active)
									{
										scavengeThisThing = false;
										break;
									}
								}
								if (uFrame >= kEHCIuFramesPerFrame)
								{
									// we made it all the way throught the 8 uFrames and found none that were marked active, so we should go ahead and scavenge this
									scavengeThisThing = true;
								}
							}
							
							if (!scavengeThisThing)
							{
								prevThing = thing;
								thing = nextThing;
								needToRescavenge = true;
								continue;
							}
						}
						// need to unlink this TD
						if (!prevThing)
						{
							SetPeriodicListEntry(testSlot, nextThing);
						}
						else
						{
							prevThing->_logicalNext = nextThing;
							prevThing->SetPhysicalLink(thing->GetPhysicalLink());
						}
						
						if (isochEl->_lowLatency)
						{
							// kprintf("EHCI::FilterInterrupt - updating isochEl(%p) curFrame(%d) curMicroFrame(%d)\n", isochEl, (int)curFrame, (int)curMicroFrame);
							isochEl->UpdateFrameList(*(AbsoluteTime*)&timeStamp);
						}
						// place this guy on the backward done queue
						// the reason that we do not use the _logicalNext link is that the done queue is not a null terminated list
						// and the element linked "last" in the list might not be a true link - trust me
						isochEl->_doneQueueLink = cachedHead;
						cachedHead = isochEl;
						cachedProducer++;
						if (isochEl->_pEndpoint)
						{
							isochEl->_pEndpoint->onProducerQ++;
							OSDecrementAtomic( &(isochEl->_pEndpoint->scheduledTDs));
						}
						
						thing = nextThing;
					}
					testSlot = nextSlot;
					if (!needToRescavenge && (testSlot != curSlot))
						_outSlot = testSlot;
				}
				IOSimpleLockLock( _wdhLock );
				
				_savedDoneQueueHead = cachedHead;	// updates the shadow head
				_producerCount = cachedProducer;	// Validates _producerCount;
				
				IOSimpleLockUnlock( _wdhLock );
			}
			// 8394970:  Make sure we set the flag AFTER we have incremented our producer count.
			_completeInterrupt = kEHCICompleteIntBit;
		}
    }
	
	if (statusClearBits)
	{
		UInt32		usbsts;
		
		_pEHCIRegisters->USBSTS = HostToUSBLong(statusClearBits);							// clear the bit again
		IOSync();
		
		// 9385815 - if we didn't do this earlier, we need to read over the PCIe bus at least once after we write the bits to clear the status
		usbsts = USBToHostLong(_pEHCIRegisters->USBSTS);
		
	}

    // We will return false from this filter routine, but will indicate that there the action routine should be called by calling _filterInterruptSource->signalInterrupt(). 
    // This is needed because IOKit will disable interrupts for a level interrupt after the filter interrupt is run, until the action interrupt is called.  We want to be
    // able to have our filter interrupt routine called before the action routine runs, if needed.  That is what will enable low latency isoch transfers to work, as when the
    // system is under heavy load, the action routine can be delayed for tens of ms.
    //
    if (needSignal)
		_filterInterruptSource->signalInterrupt();
    
    return false;
    
}

