/*
 * Copyright (c) 1998-2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

// This class' header file
#include <IOKit/usb/IOUSBMassStorageClass.h>

// Macros for printing debugging information
#if (USB_MASS_STORAGE_DEBUG == 1)
#define STATUS_LOG(x)	IOLog x
#else
#define STATUS_LOG(x)
#endif

// Bulk Only State Machine States
enum
{
	kBulkOnlyCommandSent = 1,
	kBulkOnlyBulkIOComplete,
	kBulkOnlyCheckBulkStall,
	kBulkOnlyClearBulkStall,
	kBulkOnlyStatusReceived,
	kBulkOnlyStatusReceived2ndTime,
	kBulkOnlyResetCompleted,
	kBulkOnlyClearBulkInCompleted,
	kBulkOnlyClearBulkOutCompleted
};

#pragma mark -
#pragma mark Protocol Services Methods

// The AbortSCSICommand helper method for Bulk Only protocol devices.
IOReturn IOUSBMassStorageClass::AbortSCSICommandForBulkOnlyProtocol(
                                        SCSITaskIdentifier request )
{
	return kIOReturnError;
}

// The SendSCSICommand helper method for Bulk Only protocol devices.
IOReturn IOUSBMassStorageClass::SendSCSICommandForBulkOnlyProtocol(
                                         SCSITaskIdentifier request )
{
	IOReturn					status;
	BulkOnlyRequestBlock *		theBulkOnlyRB;

	theBulkOnlyRB = GetBulkOnlyRequestBlock();
	
	// Clear out the CBW	
	bzero( theBulkOnlyRB, sizeof( BulkOnlyRequestBlock ) );

	// Save the SCSI Task
	theBulkOnlyRB->request = request; 	
	
	// Set up the IOUSBCompletion structure
	theBulkOnlyRB->boCompletion.target 		= this;
	theBulkOnlyRB->boCompletion.action 		= &this->BulkOnlyUSBCompletionAction;
	theBulkOnlyRB->boCompletion.parameter 	= theBulkOnlyRB;

   	STATUS_LOG(("%s: SendSCSICommandForBulkOnlyProtocol send CBW \n", getName()));
	status = BulkOnlySendCBWPacket( theBulkOnlyRB, kBulkOnlyCommandSent );
   	STATUS_LOG(("%s: SendSCSICommandForBulkOnlyProtocol send CBW returned %d\n", 
   				getName(), 
   				status));
   	if ( status != kIOReturnSuccess )
   	{
   		ReleaseBulkOnlyRequestBlock( theBulkOnlyRB );
   	}
   	
	return status;
}

#pragma mark -
#pragma mark Bulk Only Protocol Specific Commands

IOReturn 
IOUSBMassStorageClass::BulkDeviceResetDevice(
						BulkOnlyRequestBlock *		boRequestBlock,
						UInt32						nextExecutionState )
{
	IOReturn			status;

	if ( GetInterfaceReference() == NULL )
	{
 		// We have an invalid interface, the device has probably been removed.
 		// Nothing else to do except to report an error.
 		return kIOReturnDeviceError;
	}

	// Clear out the structure for the request
	bzero( &fUSBDeviceRequest, sizeof(IOUSBDevRequest));

	// Build the USB command	
    fUSBDeviceRequest.bmRequestType 	= USBmakebmRequestType(kUSBNone, kUSBClass, kUSBInterface);	
   	fUSBDeviceRequest.bRequest 			= 0xFF;
   	fUSBDeviceRequest.wValue			= 0;
	fUSBDeviceRequest.wIndex			= 0;
	fUSBDeviceRequest.wLength			= 0;
   	fUSBDeviceRequest.pData				= NULL;

	// Set the next state to be executed
	boRequestBlock->currentState = nextExecutionState;

	// Send the command over the control endpoint
	status = GetInterfaceReference()->DeviceRequest( &fUSBDeviceRequest, &boRequestBlock->boCompletion );
   	STATUS_LOG(("%s: BulkDeviceResetDevice returned %d\n", 
   				getName(), 
   				status));
	return status;
}


#pragma mark -
#pragma mark SendSCSICommand Helper methods

void 
IOUSBMassStorageClass::BulkOnlyUSBCompletionAction(
					                void *			target,
					                void *			parameter,
					                IOReturn		status,
					                UInt32			bufferSizeRemaining)
{
	IOUSBMassStorageClass *		theMSC;
	BulkOnlyRequestBlock *		boRequestBlock;
	
	theMSC 			= (IOUSBMassStorageClass *) target;
	boRequestBlock 	= (BulkOnlyRequestBlock *) parameter;
	theMSC->BulkOnlyExecuteCommandCompletion( 	boRequestBlock, 
												status, 
												bufferSizeRemaining );
}

// Prepare the Command Block Wrapper packet for Bulk Only Protocol
IOReturn 
IOUSBMassStorageClass::BulkOnlySendCBWPacket(
						BulkOnlyRequestBlock *		boRequestBlock,
						UInt32						nextExecutionState )
{
	IOReturn 			status;

	// Allocate the memory descriptor needed to send the CBW out
	boRequestBlock->boPhaseDesc = IOMemoryDescriptor::withAddress( 
										&boRequestBlock->boCBW, 
										kByteCountOfCBW, 
										kIODirectionOut);
	if ( boRequestBlock->boPhaseDesc == NULL )
	{
		// The memory descriptor could not be allocated and so the command
		// can not be sent to the device, return an error.
		return kIOReturnNoResources;
	}
	
	boRequestBlock->boCBW.cbwSignature 			= kCommandBlockWrapperSignature;
	boRequestBlock->boCBW.cbwTag 				= GetNextBulkOnlyCommandTag();
	boRequestBlock->boCBW.cbwTransferLength 	= HostToUSBLong(
						GetRequestedDataTransferCount(boRequestBlock->request));
	if (GetDataTransferDirection(boRequestBlock->request) == 
							kSCSIDataTransfer_FromTargetToInitiator)
	{
		boRequestBlock->boCBW.cbwFlags 		= kCBWFlagsDataIn;
	}
	else if (GetDataTransferDirection(boRequestBlock->request) == 
							kSCSIDataTransfer_FromInitiatorToTarget)
	{
		boRequestBlock->boCBW.cbwFlags 		= kCBWFlagsDataOut;
	}
	else
	{
		boRequestBlock->boCBW.cbwFlags 		= 0;
	}

	// Set the LUN (not needed until LUN support is added).
	boRequestBlock->boCBW.cbwLUN 			= GetLogicalUnitNumber( boRequestBlock->request ) & kCBWLUNMask;				// Bits 0-3: LUN, 4-7: Reserved
	boRequestBlock->boCBW.cbwCDBLength 		= GetCommandDescriptorBlockSize( boRequestBlock->request );		// Bits 0-4: CDB Length, 5-7: Reserved
	GetCommandDescriptorBlock( boRequestBlock->request, &boRequestBlock->boCBW.cbwCDB );

	// Once timeouts are support, set the timeout value for the request 

	// Set the next state to be executed
	boRequestBlock->currentState = nextExecutionState;

	// Send the CBW to the device	
	if ( GetBulkOutPipe() == NULL )
	{
   		STATUS_LOG(("%s: BulkOnlySendCBWPacket Bulk Out is NULL\n", getName() ));
	}
	
   	STATUS_LOG(("%s: BulkOnlySendCBWPacket sent\n", getName() ));
	status = GetBulkOutPipe()->Write( boRequestBlock->boPhaseDesc, &boRequestBlock->boCompletion );
   	STATUS_LOG(("%s: BulkOnlySendCBWPacket returned %d\n", 
   				getName(), 
   				status));
	return status;
}

IOReturn 
IOUSBMassStorageClass::BulkOnlyTransferData( 
						BulkOnlyRequestBlock *		boRequestBlock,
						UInt32						nextExecutionState )
{
	IOReturn	status = kIOReturnError;
	
	// Set the next state to be executed
	boRequestBlock->currentState = nextExecutionState;

	// Start a bulk in or out transaction
	if (GetDataTransferDirection(boRequestBlock->request) == kSCSIDataTransfer_FromTargetToInitiator)
	{
		status = GetBulkInPipe()->Read(
					GetDataBuffer( boRequestBlock->request ), 
					GetTimeoutDuration( boRequestBlock->request ),  // Use the client's timeout for both
					GetTimeoutDuration( boRequestBlock->request ),
					&boRequestBlock->boCompletion );
	}
	else if (GetDataTransferDirection(boRequestBlock->request) == kSCSIDataTransfer_FromInitiatorToTarget)
	{
		status = GetBulkOutPipe()->Write(
					GetDataBuffer( boRequestBlock->request ), 
					GetTimeoutDuration( boRequestBlock->request ),  // Use the client's timeout for both
					GetTimeoutDuration( boRequestBlock->request ),
					&boRequestBlock->boCompletion );
	}

   	STATUS_LOG(("%s: BulkOnlyTransferData returned %d\n", 
   				getName(), 
   				status));
	return status;
}

// Prepare the Command Status Wrapper packet for Bulk Only Protocol
IOReturn 
IOUSBMassStorageClass::BulkOnlyReceiveCSWPacket(
						BulkOnlyRequestBlock *		boRequestBlock,
						UInt32						nextExecutionState )
{
	IOReturn 			status;

	// Allocate the memory descriptor needed to send the CBW out
	boRequestBlock->boPhaseDesc = IOMemoryDescriptor::withAddress(&boRequestBlock->boCSW, 
					kByteCountOfCSW, 
					kIODirectionIn);
	if ( boRequestBlock->boPhaseDesc == NULL )
	{
		// The memory descriptor could not be allocated and so the command
		// can not be sent to the device, return an error.
		return kIOReturnNoResources;
	}
	
	// Set the next state to be executed
	boRequestBlock->currentState = nextExecutionState;

	// Retrieve the CSW from the device	
	status = GetBulkInPipe()->Read( boRequestBlock->boPhaseDesc, &boRequestBlock->boCompletion );

   	STATUS_LOG(("%s: BulkOnlyReceiveCSWPacket returned %d\n", 
   				getName(), 
   				status));
	return status;
}

void 
IOUSBMassStorageClass::BulkOnlyExecuteCommandCompletion(
						BulkOnlyRequestBlock *	boRequestBlock,
		                IOReturn				resultingStatus,
		                UInt32					bufferSizeRemaining)
{
	IOReturn 		status = kIOReturnError;
	bool			commandInProgress = false;

	if ( boRequestBlock->request == NULL )
	{
		// The request field is NULL, this appears to
		// be a double callback, do nothing.
		STATUS_LOG(("%s: boRequestBlock->request is NULL, returned %d\n", getName(), resultingStatus));
		return;
	}
	
	if ( GetInterfaceReference() == NULL )
	{
		// Our interface has been closed, probably because of an
		// unplug, return an error for the command since it can no 
		// longer be executed.
		SCSITaskIdentifier	request = boRequestBlock->request;
		
		STATUS_LOG(("%s: Interface object is NULL, returned %d\n", getName(), resultingStatus));

		ReleaseBulkOnlyRequestBlock( boRequestBlock );
		CompleteSCSICommand( request, status );
		return;
	}		
	
	switch( boRequestBlock->currentState )
	{
		case kBulkOnlyCommandSent:
		{
   			STATUS_LOG(("%s: kBulkOnlyCommandSent returned %d\n", getName(), resultingStatus));

			// Release the memory descriptor for the CBW
			boRequestBlock->boPhaseDesc->release();

			// If there is to be no data transfer then we are done and can return to the caller
			if ( GetDataTransferDirection( boRequestBlock->request ) == kSCSIDataTransfer_NoDataTransfer )
			{
				if ( resultingStatus != kIOReturnSuccess)
				{
					status = resultingStatus;
				}
				else
				{
					// Bulk transfer is done, get the Command Status Wrapper from the device
					status = BulkOnlyReceiveCSWPacket( boRequestBlock, kBulkOnlyStatusReceived );
					if ( status == kIOReturnSuccess )
					{
						commandInProgress = true;
					}
				}
				
				// There is no data to be transfered
				break;
			}
			
			if ( resultingStatus != kIOReturnSuccess )
			{
				// An error occurred, probably a timeout error,
				// and the command was not successfully sent to the device.
				status = resultingStatus;
				break;
			}


			// Start a bulk in or out transaction
			status = BulkOnlyTransferData( boRequestBlock, kBulkOnlyBulkIOComplete ); 
			if ( status == kIOReturnSuccess )
			{
				commandInProgress = true;
			}
		}
		break;
		
		case kBulkOnlyBulkIOComplete:
		{
			//cmdPBPtr->commandPB.actualCount 	+=	usbPB->usbActCount;			// Update the users byte count
			status 		=	resultingStatus;			// and status

   			STATUS_LOG(("%s: kBulkOnlyBulkIOComplete returned %d\n", getName(), resultingStatus));
			
			if ( resultingStatus == kIOReturnSuccess)
			{
				{
					// Bulk transfer is done, get the Command Status Wrapper from the device
					status = BulkOnlyReceiveCSWPacket( boRequestBlock, kBulkOnlyStatusReceived );
					if ( status == kIOReturnSuccess )
					{
						commandInProgress = true;
					}
				}
			}
			else
			{
				// Either an error occurred on transfer or we did not get all the data we requested.
				// In either case, this transfer is complete, clean up and return an error to the client.
				// Check if the bulk endpoint was stalled
				IOUSBPipe * thePipe = NULL;

				if ( GetDataTransferDirection( boRequestBlock->request ) == kSCSIDataTransfer_FromTargetToInitiator )
				{
					thePipe = GetBulkInPipe();
				}
				else if ( GetDataTransferDirection( boRequestBlock->request ) == kSCSIDataTransfer_FromInitiatorToTarget )
				{
					thePipe = GetBulkOutPipe();
				}
				else
				{
					thePipe = GetControlPipe();
				}
				
				boRequestBlock->currentState = kBulkOnlyCheckBulkStall;
				status = GetStatusEndpointStatus( thePipe, &boRequestBlock->boGetStatusBuffer, &boRequestBlock->boCompletion );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
		}
		break;

		case kBulkOnlyCheckBulkStall:
		{
   			STATUS_LOG(("%s: kBulkOnlyCheckBulkStall returned %d\n", getName(), resultingStatus));

			// Check to see if the endpoint was stalled
			if ( (boRequestBlock->boGetStatusBuffer[0] & 1) == 1 )
			{
				IOUSBPipe * thePipe = NULL;
				
				if ( GetDataTransferDirection( boRequestBlock->request ) == kSCSIDataTransfer_FromTargetToInitiator )
				{
					thePipe = GetBulkInPipe();
				}
				else if ( GetDataTransferDirection( boRequestBlock->request ) == kSCSIDataTransfer_FromInitiatorToTarget )
				{
					thePipe = GetBulkOutPipe();
				}
				else
				{
					thePipe = GetControlPipe();
				}
				
				boRequestBlock->currentState = kBulkOnlyClearBulkStall;
				status = ClearFeatureEndpointStall( thePipe, &boRequestBlock->boCompletion );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
			else
			{
				// If the endpoint was not stalled, attempt to get the CSW
				status = BulkOnlyReceiveCSWPacket( boRequestBlock, kBulkOnlyStatusReceived );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
		}
		break;
		
		case kBulkOnlyClearBulkStall:
		{
   			STATUS_LOG(("%s: kBulkOnlyClearBulkStall returned %d\n", getName(), resultingStatus));

			// The pipe was stalled and an attempt to clear it was made
			// Try to get the CSW.  If the pipe was not successfully cleared, this will also
			// set off a device reset sequence
			status = BulkOnlyReceiveCSWPacket( boRequestBlock, kBulkOnlyStatusReceived );
			if ( status == kIOReturnSuccess )
			{
				commandInProgress = true;
			}
		}
		break;
		
		case kBulkOnlyStatusReceived:
		{
   			STATUS_LOG(("%s: kBulkOnlyStatusReceived returned %d\n", getName(), resultingStatus));
			
			// Release the memory descriptor for the CSW
			boRequestBlock->boPhaseDesc->release();

			// Bulk transfer is done, get the Command Status Wrapper from the device
			if ( resultingStatus == kIOUSBPipeStalled)
			{
				// The device reported an error on the command
				// report an error to the client
				boRequestBlock->currentState = kBulkOnlyCheckBulkStall;
				status = GetStatusEndpointStatus( GetBulkInPipe(), &boRequestBlock->boGetStatusBuffer, &boRequestBlock->boCompletion );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
			else if ( resultingStatus != kIOReturnSuccess)
			{
				// An error occurred trying to get the first CSW, we should check and clear the stall,
				// and then try the CSW again
				status = BulkOnlyReceiveCSWPacket( boRequestBlock, kBulkOnlyStatusReceived2ndTime );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
			else if( boRequestBlock->boCSW.cswTag == boRequestBlock->boCBW.cbwTag) 
			{
				// Since the CBW and CSW tags match, process t
				// Process the CSW and determine appropriate response
				switch( boRequestBlock->boCSW.cswStatus )
				{
					case kCSWCommandPassedError:
					{
						// The device reports no error on the command, and the command has
						// the same tag as the command that was sent, check to make sure all data was retrieved
						if( SetRealizedDataTransferCount( boRequestBlock->request, GetRequestedDataTransferCount( boRequestBlock->request )) )
						{
							// We were able to get all the data for the device
							status = kIOReturnSuccess;
						}
						else
						{
							// An error occurred and we did not get all the data
							status = kIOUSBPipeStalled;
						}
					}
					break;
					
					case kCSWCommandFailedError:
					{
						// The device reported an error for the command.
						status = kIOReturnError;
					}
					break;
					
					case kCSWPhaseError:
					{
						// The device reported a phase error on the command, perform the 
						// bulk reset on the device.
						status = BulkDeviceResetDevice( boRequestBlock, kBulkOnlyResetCompleted );
						if( status == kIOReturnSuccess )
						{
							commandInProgress = true;
						}
					}
					break;
					
					default:
					{
						// We received an unkown status, report an error to the client.
						status = kIOReturnError;
					}
					break;
				}
			}
			else
			{
				// The only way to get to this point is if the command completes successfully,
				// but the CBW and CSW tags do not match.  Report an error to the client.
				status = kIOReturnError;
			}
		}
		break;
		
		case kBulkOnlyStatusReceived2ndTime:
		{
   			STATUS_LOG(("%s: kBulkOnlyStatusReceived2ndTime returned %d\n", getName(), resultingStatus));

			// Second try for the CSW is done, if an error occurred, reset device.
			if ( resultingStatus != kIOReturnSuccess)
			{
				status = BulkDeviceResetDevice( boRequestBlock, kBulkOnlyResetCompleted );
				if ( status == kIOReturnSuccess )
				{
					commandInProgress = true;
				}
			}
			else
			{
				status = kIOReturnError;
			}
		}
		break;
			
		case kBulkOnlyResetCompleted:
		{
   			STATUS_LOG(("%s: kBulkOnlyResetCompleted returned %d\n", getName(), resultingStatus));

			boRequestBlock->currentState = kBulkOnlyClearBulkInCompleted;
			status = ClearFeatureEndpointStall( GetBulkInPipe(), &boRequestBlock->boCompletion );
			if ( status == kIOReturnSuccess )
			{
				commandInProgress = true;
			}
		}
		break;

		case kBulkOnlyClearBulkInCompleted:
		{
   			STATUS_LOG(("%s: kBulkOnlyClearBulkInCompleted returned %d\n", getName(), resultingStatus));

			boRequestBlock->currentState = kBulkOnlyClearBulkOutCompleted;
			status = ClearFeatureEndpointStall( GetBulkOutPipe(), &boRequestBlock->boCompletion );
			if ( status == kIOReturnSuccess )
			{
				commandInProgress = true;
			}
		}
		break;
		
		case kBulkOnlyClearBulkOutCompleted:
		{
   			STATUS_LOG(("%s: kBulkOnlyClearBulkOutCompleted returned %d\n", getName(), resultingStatus));

			SetRealizedDataTransferCount( boRequestBlock->request, 0 );
			status = kIOReturnError;
		}
		break;
		
		default:
		{
			SetRealizedDataTransferCount( boRequestBlock->request, 0 );
			status = kIOReturnError;
		}
		break;
	}
	
	if ( commandInProgress == false )
	{
		SCSITaskIdentifier	request = boRequestBlock->request;
		
		ReleaseBulkOnlyRequestBlock( boRequestBlock );
		CompleteSCSICommand( request, status );
	}
}
