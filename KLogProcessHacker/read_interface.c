//----------------------------------------------------------------------------
// Provides an interface for userspace programs to read PCAP-NG blocks
// collected by the Hone driver
//
// Copyright (c) 2014-2015 Battelle Memorial Institute
// Licensed under a modification of the 3-clause BSD license
// See License.txt for the full text of the license and additional disclaimers
//
// Authors
//   Alexis J. Malozemoff <alexis.malozemoff@pnnl.gov>
//   Peter L. Nordquist <peter.nordquist@pnnl.gov>
//   Richard L. Griswold <richard.griswold@pnnl.gov>
//   Ruslan A. Doroshchuk <ruslan.doroshchuk@pnnl.gov>
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// Includes
//----------------------------------------------------------------------------

#include "read_interface_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------
// Global variables
//----------------------------------------------------------------------------

// These must be in the same order that the IOCTLS are declared
static const IOCTL_PARAMS gIoctlParams[] =
{
	{ 0,              0,     0,              0     }, // IoctlRestart
	{ 0,              0,     0,              0     }, // IoctlFilterConnections
	{ 0,              0,     0,              0     }, // IoctlFilterProcesses
	{ sizeof(UINT32), 0,     sizeof(UINT32), 0     }, // IoctlSetSnapLength
	{ 0, sizeof(UINT32),     0, sizeof(UINT32)     }, // IoctlGetSnapLength
	{ sizeof(UINT32), 0,     sizeof(UINT64), 0     }, // IoctlSetDataEvent
	{ sizeof(UINT32), 0,     sizeof(UINT32), 0     }, // IoctlOpenConnections
	{ 0, sizeof(STATISTICS), 0, sizeof(STATISTICS) }, // IoctlGetStatistics
};

static LOOKASIDE_LIST_EX gLookasideList;              // Holds memory for netbuffer storage
static bool              gLookasideListInit = false;  // True if lookaside list was initialized
static const UINT32      gPoolTagIds        = 'ihpK'; // Tag to use when allocating ID lists
static const UINT32      gPoolTagLookaside  = 'LHpK'; // Tag to use when allocating lookaside buffers

//----------------------------------------------------------------------------
static inline NTSTATUS CompleteIrp(
	__in PIRP      irp,
	__in NTSTATUS  status,
	__in ULONG_PTR information)
{
	irp->IoStatus.Status      = status;
	irp->IoStatus.Information = information;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}

//----------------------------------------------------------------------------
__checkReturn
NTSTATUS DeinitializeReadInterface(void)
{
	if (gLookasideListInit) {
		ExDeleteLookasideListEx(&gLookasideList);
	}
	return STATUS_SUCCESS;
}

//----------------------------------------------------------------------------
NTSTATUS DispatchClose(__in PDEVICE_OBJECT deviceObject, __inout PIRP irp)
{
	IO_STACK_LOCATION *irpSp = IoGetCurrentIrpStackLocation(irp);
	READER_CONTEXT    *context;

	UNREFERENCED_PARAMETER(deviceObject);
	context = (READER_CONTEXT*)(irpSp->FileObject->FsContext2);
	if (context == NULL) {
		return CompleteIrp(irp, STATUS_INVALID_PARAMETER, NULL);
	}

	QmDeregisterReader(&context->Reader);
	if (context->CurrentBlock) {
		QmCleanupBlock(context->CurrentBlock);
	}
	if (context->FilteredConnectionIds) {
		ExFreePool(context->FilteredConnectionIds);
	}
	if (context->FilteredProcessIds) {
		ExFreePool(context->FilteredProcessIds);
	}
	ExFreeToLookasideListEx(&gLookasideList, context);
	return CompleteIrp(irp, STATUS_SUCCESS, NULL);
}

//----------------------------------------------------------------------------
NTSTATUS DispatchCreate(__in PDEVICE_OBJECT deviceObject, __inout PIRP irp)
{
	NTSTATUS           status  = STATUS_SUCCESS;
	IO_STACK_LOCATION *irpSp   = NULL;
	DEVICE_EXTENSION  *devExt  = NULL;
	READER_CONTEXT    *context = NULL;

	// Sanity checks to ensure that:
	//  * We are in the same process context as the caller
	//  * Caller didn't open us with a path (for example \\.\Queues\foo)
	//  * We have a device extension
	if (PsGetCurrentThread() != irp->Tail.Overlay.Thread) {
		status = STATUS_ACCESS_DENIED;
		goto Cleanup;
	}
	irpSp = IoGetCurrentIrpStackLocation(irp);
	if (irpSp->FileObject->FileName.Length) {
		status = STATUS_NO_SUCH_FILE;
		goto Cleanup;
	}
	devExt = (DEVICE_EXTENSION*)(deviceObject->DeviceExtension);
	if (devExt == NULL) {
		status = STATUS_INVALID_PARAMETER;
		goto Cleanup;
	}

	context = (READER_CONTEXT*)(ExAllocateFromLookasideListEx(&gLookasideList));
	if (context == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto Cleanup;
	}

	RtlZeroMemory(context, sizeof(READER_CONTEXT));
	context->DeviceExtension = devExt;
	status = QmRegisterReader(&context->Reader);
	if (!NT_SUCCESS(status)) {
		goto Cleanup;
	}
	irpSp->FileObject->FsContext2 = context;

Cleanup:
	if (!NT_SUCCESS(status)) {
		DBGPRINT(D_WARN, "Open reader failed: %08X", status);
		if (context) {
			ExFreeToLookasideListEx(&gLookasideList, context);
		}
	}
	return CompleteIrp(irp, status, NULL);
}

//----------------------------------------------------------------------------
NTSTATUS DispatchDeviceControl(
	__in PDEVICE_OBJECT deviceObject,
	__inout PIRP        irp)
{
	NTSTATUS           status       = STATUS_SUCCESS;
	IO_STACK_LOCATION *irpSp        = IoGetCurrentIrpStackLocation(irp);
	READER_CONTEXT    *context      = NULL;
	void              *buffer       = irp->AssociatedIrp.SystemBuffer;
	const UINT32       inBufLen     = irpSp->Parameters.DeviceIoControl.InputBufferLength;
	UINT32             inBufLenReq;
	const UINT32       outBufLen    = irpSp->Parameters.DeviceIoControl.OutputBufferLength;
	UINT32             outBufLenReq;
	const UINT32       ioctl        = irpSp->Parameters.DeviceIoControl.IoControlCode;
	const UINT32       function     = (ioctl & 0x0ffc) >> 2;
	const bool         is64Bit      = (ioctl & 0x1000) ? true : false;
	UINT32             bytesOut     = 0;

	UNREFERENCED_PARAMETER(deviceObject);

	context = (READER_CONTEXT*)irpSp->FileObject->FsContext2;
	if (context == NULL) {
		return CompleteIrp(irp, STATUS_INVALID_PARAMETER, NULL);
	}

	DBGPRINT(D_INFO, "Received IOCTL %08X: input len %08X, output len %08X",
			ioctl, inBufLen, outBufLen);

	// Check buffer sizes
	if (function > ARRAY_SIZEOF(gIoctlParams)) {
		return CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, NULL);
	}
	if (is64Bit) {
		inBufLenReq  = gIoctlParams[function].InputLength64;
		outBufLenReq = gIoctlParams[function].OutputLength64;
	} else {
		inBufLenReq  = gIoctlParams[function].InputLength;
		outBufLenReq = gIoctlParams[function].OutputLength;
	}
	if ((inBufLen < inBufLenReq) || (outBufLen < outBufLenReq)) {
		return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL, NULL);
	}
	if ((inBufLenReq || outBufLenReq) && !buffer) {
		return CompleteIrp(irp, STATUS_INVALID_PARAMETER, NULL);
	}

	switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_KPH_FILTER_CONNECTIONS:
		SetIdList(context, ConnectionIdList, buffer, inBufLen);
		break;
	case IOCTL_KPH_FILTER_PROCESSES:
		SetIdList(context, ProcessIdList, buffer, inBufLen);
		break;
	case IOCTL_KPH_MARK_RESTART:
		context->RestartRequested = 1;
		DBGPRINT(D_INFO, "Restarting reader %d", context->Reader.Id);
		break;
	case IOCTL_KPH_SET_SNAP_LENGTH:
	{
		const UINT32 snapLength = *(const UINT32*)buffer;
		if (context->SnapLength != snapLength) {
			// Notify the queue manager of the snap length change so it can
			// recalculate its maximum snap length
			context->SnapLength    = snapLength;
			context->SnapLengthPad = PCAP_NG_PADDING(snapLength) - snapLength;
			QmSetReaderSnapLength(&context->Reader, context->SnapLength);
		}
		DBGPRINT(D_INFO, "Set snap length to %08X (%d) for reader %d",
				context->SnapLength, context->SnapLength, context->Reader.Id);
		break;
	}
	case IOCTL_KPH_GET_SNAP_LENGTH:
		*(UINT32*)buffer = context->SnapLength;
		DBGPRINT(D_INFO, "Get snap length of %08X (%d) for reader %d",
				context->SnapLength, context->SnapLength, context->Reader.Id);
		bytesOut = outBufLenReq;
		break;
	case IOCTL_KPH_SET_DATA_EVENT_32:
	{
		const UINT32 event       = *(UINT32*)buffer;
		const HANDLE eventHandle = (const HANDLE)event;
		status = QmSetReaderDataEvent(&context->Reader, eventHandle);
		DBGPRINT(D_INFO, "%s data notification for reader %d", eventHandle ?
				"Enabling" : "Disabling", context->Reader.Id);
		break;
	}
	case IOCTL_KPH_SET_DATA_EVENT_64:
#ifdef _X86_
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
#else
	{
		const UINT64 event       = *(UINT64*)buffer;
		const HANDLE eventHandle = (const HANDLE)event;
		status = QmSetReaderDataEvent(&context->Reader, eventHandle);
		DBGPRINT(D_INFO, "%s data notification for reader %d", eventHandle ?
				"Enabling" : "Disabling", context->Reader.Id);
		break;
	}
#endif
	case IOCTL_KPH_SET_OPEN_CONNECTIONS:
		QmSetOpenConnections((CONNECTIONS*)buffer);
		break;
	case IOCTL_KPH_GET_STATISTICS:
		QmGetStatistics((STATISTICS*)buffer, &context->Reader);
		bytesOut = outBufLenReq;
		break;
	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	return CompleteIrp(irp, status, bytesOut);
}

//----------------------------------------------------------------------------
NTSTATUS DispatchRead(
	__in PDEVICE_OBJECT deviceObject,
	__inout PIRP        irp)
{
	BLOCK_NODE        *blockNode    = NULL;
	UINT32             blockOffset  = 0;
	IO_STACK_LOCATION *irpSp        = IoGetCurrentIrpStackLocation(irp);
	READER_CONTEXT    *context      = NULL;
	UINT8             *readBuffer   = NULL;
	UINT32             readLength   = 0;
	UINT32             readOffset   = 0;

	UNREFERENCED_PARAMETER(deviceObject);

	// Verify the buffer and open instance aren't NULL
	readBuffer = (UINT8*)irp->AssociatedIrp.SystemBuffer;
	if (readBuffer == NULL) {
		return CompleteIrp(irp, STATUS_INVALID_PARAMETER, NULL);
	}
	context = (READER_CONTEXT*)irpSp->FileObject->FsContext2;
	if (context == NULL) {
		return CompleteIrp(irp, STATUS_INVALID_PARAMETER, NULL);
	}

	// Check restart state
	switch (context->RestartState) {
	case RestartStateSendEof:
		// Return zero bytes to tell reader it's at a block boundary
		context->RestartState = RestartStateInit;
		return CompleteIrp(irp, STATUS_SUCCESS, NULL);
	case RestartStateInit:
		// Get initial PCAP-NG blocks
		QmGetInitialBlocks(&context->Reader, false);
		context->RestartState = RestartStateNormal;
		break;
	case RestartStateNormal:
		break;
	}

	readLength  = irpSp->Parameters.Read.Length;
	blockNode   = context->CurrentBlock;
	blockOffset = context->CurrentBlockOffset;
	while (readOffset < readLength) {
		char   *blockData   = NULL;
		UINT32  blockLength = 0;
		UINT32  bytesToCopy = 0;

		if (!blockNode) {
			// Handle restart request now that we're at a block boundary
			if (InterlockedCompareExchange(&context->RestartRequested, 0, 1) == 1) {
				context->RestartState = readOffset ?
						RestartStateSendEof : RestartStateInit;
				break;
			}

			blockNode   = QmDequeueBlock(&context->Reader);
			blockOffset = 0;
			if (!blockNode) {
				break;  // No more blocks
			}
			context->ModifiedHeader.BlockType = 0; // Not trimming packet block

			if (blockNode->BlockType == PacketBlock) {
				PCAP_NG_PACKET_HEADER *header;

				// Filter this block if filtering the connection or process ID
				bool filter = false;
				if (context->FilteredProcessIds) {
					for (UINT32 index = 1; index <= context->FilteredProcessIds[0]; index++) {
						DBGPRINT(D_DBG, "Checking filter %d of %d: block process ID %d (%08X), filter process ID %d (%08X)",
								index, context->FilteredProcessIds[0],
								blockNode->ProcessId, blockNode->ProcessId,
								context->FilteredProcessIds[index], context->FilteredProcessIds[index]);
						if (context->FilteredProcessIds[index] == blockNode->ProcessId) {
							filter = true;
							break;
						}
					}
				}
				if (!filter && context->FilteredConnectionIds) {
					for (UINT32 index = 1; index <= context->FilteredConnectionIds[0]; index++) {
						DBGPRINT(D_DBG, "Checking filter %d of %d: block connection ID %d (%08X), filter connection ID %d (%08X)",
								index, context->FilteredConnectionIds[0],
								blockNode->ConnectionId, blockNode->ConnectionId,
								context->FilteredConnectionIds[index], context->FilteredConnectionIds[index]);
						if (context->FilteredConnectionIds[index] == blockNode->ConnectionId) {
							filter = true;
							break;
						}
					}
				}
				if (filter) {
					DBGPRINT(D_INFO, "Filtering packet for process %08X, connection %08X",
							blockNode->ProcessId, blockNode->ConnectionId);
					QmCleanupBlock(blockNode);
					blockNode = NULL;
					continue;
				}

				// Trim block to snap length
				blockData = blockNode->Buffer ? blockNode->Buffer : blockNode->Data;
				header    = (PCAP_NG_PACKET_HEADER*)blockData;
				if (context->SnapLength && (header->CapturedLength > context->SnapLength)) {
					// Fix up packet header and footer
					context->DataEndOffset        = sizeof(PCAP_NG_PACKET_HEADER) + context->SnapLength;
					context->ModifiedFooterOffset = context->DataEndOffset + context->SnapLengthPad;
					context->OriginalFooterOffset = header->BlockLength - sizeof(PCAP_NG_PACKET_FOOTER);
					RtlCopyMemory(&context->ModifiedHeader, blockData, sizeof(PCAP_NG_PACKET_HEADER));
					RtlCopyMemory(&context->ModifiedFooter, blockData + context->OriginalFooterOffset, sizeof(PCAP_NG_PACKET_FOOTER));
					context->ModifiedHeader.BlockLength    = context->ModifiedFooterOffset + sizeof(PCAP_NG_PACKET_FOOTER);
					context->ModifiedHeader.CapturedLength = context->SnapLength;
					context->ModifiedFooter.BlockLength    = context->ModifiedHeader.BlockLength;
				}
			}
		}

		// Handle truncated packet blocks
		blockData   = blockNode->Buffer ? blockNode->Buffer : blockNode->Data;
		blockLength = blockNode->BlockLength;
		if (context->ModifiedHeader.BlockType) {
			// Copy fixed-up packet header
			if (blockOffset < sizeof(PCAP_NG_PACKET_HEADER)) {
				bytesToCopy = min(readLength - readOffset,
						sizeof(PCAP_NG_PACKET_HEADER) - blockOffset);
				DBGPRINT(D_DBG,
						"Copying %08X bytes of packet header from %08X/%08X to %08X/%08X",
						bytesToCopy, blockOffset, blockLength, readOffset, readLength);
				RtlCopyMemory(readBuffer + readOffset, (UINT8*)(&context->ModifiedHeader) + blockOffset, bytesToCopy);
				readOffset  += bytesToCopy;
				blockOffset += bytesToCopy;
			}

			// Copy truncated packet data
			if ((blockOffset >= sizeof(PCAP_NG_PACKET_HEADER)) &&
					(blockOffset < context->DataEndOffset) &&
					(readOffset  < readLength)) {
				bytesToCopy = min(readLength - readOffset,
						context->DataEndOffset - blockOffset);
				DBGPRINT(D_DBG,
						"Copying %08X bytes of packet data from %08X/%08X to %08X/%08X",
						bytesToCopy, blockOffset, blockLength, readOffset, readLength);
				RtlCopyMemory(readBuffer + readOffset, blockData + blockOffset,
						bytesToCopy);
				readOffset  += bytesToCopy;
				blockOffset += bytesToCopy;
			}

			// Pad truncated packet data
			if ((blockOffset >= context->DataEndOffset) &&
					(blockOffset < context->OriginalFooterOffset) &&
					(readOffset  < readLength)) {
				bytesToCopy = min(readLength - readOffset,
						context->ModifiedFooterOffset - blockOffset);
				DBGPRINT(D_DBG, "Copying %08X bytes of padding to %08X/%08X",
						bytesToCopy, readOffset, readLength);
				RtlZeroMemory(readBuffer + readOffset, bytesToCopy);
				readOffset  += bytesToCopy;
				blockOffset += bytesToCopy;

				// Skip rest of packet data
				if (blockOffset >= context->ModifiedFooterOffset) {
					blockOffset = context->OriginalFooterOffset;
				}
			}

			// Copy fixed-up packet footer
			if ((blockOffset >= context->OriginalFooterOffset) &&
					(readOffset  < readLength)) {
				bytesToCopy = min(readLength - readOffset, blockLength - blockOffset);
				DBGPRINT(D_DBG,
						"Copying %08X bytes of packet footer from %08X/%08X to %08X/%08X",
						bytesToCopy, blockOffset, blockLength, readOffset, readLength);
				RtlCopyMemory(readBuffer + readOffset,
						(UINT8*)(&context->ModifiedFooter) +
						(blockOffset - context->OriginalFooterOffset), bytesToCopy);
				readOffset  += bytesToCopy;
				blockOffset += bytesToCopy;
			}
		} else {
			bytesToCopy = min(readLength - readOffset, blockLength - blockOffset);
			DBGPRINT(D_DBG, "Copying %08X bytes from %08X/%08X to %08X/%08X",
					bytesToCopy, blockOffset, blockLength, readOffset, readLength);
			RtlCopyMemory(readBuffer + readOffset, blockData + blockOffset,
					bytesToCopy);
			readOffset  += bytesToCopy;
			blockOffset += bytesToCopy;
		}

		if (blockOffset >= blockLength) {
			QmCleanupBlock(blockNode);
			blockNode   = NULL;
			blockOffset = 0;
		}
	}

	context->CurrentBlock       = blockNode;
	context->CurrentBlockOffset = blockOffset;
	return CompleteIrp(irp, STATUS_SUCCESS, readOffset);
}

//----------------------------------------------------------------------------
__checkReturn
NTSTATUS InitializeReadInterface(DEVICE_OBJECT *device)
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(device);
	status = ExInitializeLookasideListEx(&gLookasideList, NULL, NULL,
			NonPagedPool, 0, sizeof(READER_CONTEXT), gPoolTagLookaside, 0);
	if (!NT_SUCCESS(status)) {
		DBGPRINT(D_ERR, "Cannot create lookaside list");
		return status;
	}
	gLookasideListInit = true;
	return STATUS_SUCCESS;
}

//----------------------------------------------------------------------------
void SetIdList(
		READER_CONTEXT     *context,
		const ID_LIST_TYPE  idListType,
		const void         *buffer,
		const UINT32        bufferLen)
{
	const char  *label;
	const char  *labelPs;
	void       **target;
	if (idListType == ConnectionIdList) {
		label = "connection";
		labelPs = "connection(s)";
		target = (void**)(&context->FilteredConnectionIds);
	} else {
		label = "process";
		labelPs = "process(es)";
		target = (void**)(&context->FilteredProcessIds);
	}

	UINT32       *ids    = NULL;
	const UINT32  numIds = bufferLen / sizeof(UINT32);
	DBGPRINT(D_INFO, "Filtering %d %s for reader %d", numIds, labelPs, context->Reader.Id);
	if (numIds) {
		ids = (UINT32*)(ExAllocatePoolWithTag(NonPagedPool, (numIds + 1) * sizeof(UINT32), gPoolTagIds));
		ids[0] = numIds;
		const UINT32 *newIds = (const UINT32*)buffer;
		for (UINT32 index = 0; index < numIds; index++) {
			const UINT32 id = newIds[index];
			DBGPRINT(D_INFO, "Filtering %s %08X (%d) for reader %d", label, id, id, context->Reader.Id);
			ids[index + 1] = id;
		}
	}

	UINT32 *currentIds = (UINT32*)(InterlockedExchangePointer(target, ids));
	if (currentIds) {
		ExFreePool(currentIds);
	}
}

#ifdef __cplusplus
};
#endif
