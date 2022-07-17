/**********************************************************************************
 *                                                                                *
 * BthPS3 - Windows kernel-mode Bluetooth profile and bus driver                  *
 *                                                                                *
 * BSD 3-Clause License                                                           *
 *                                                                                *
 * Copyright (c) 2018-2022, Nefarius Software Solutions e.U.                      *
 * All rights reserved.                                                           *
 *                                                                                *
 * Redistribution and use in source and binary forms, with or without             *
 * modification, are permitted provided that the following conditions are met:    *
 *                                                                                *
 * 1. Redistributions of source code must retain the above copyright notice, this *
 *    list of conditions and the following disclaimer.                            *
 *                                                                                *
 * 2. Redistributions in binary form must reproduce the above copyright notice,   *
 *    this list of conditions and the following disclaimer in the documentation   *
 *    and/or other materials provided with the distribution.                      *
 *                                                                                *
 * 3. Neither the name of the copyright holder nor the names of its               *
 *    contributors may be used to endorse or promote products derived from        *
 *    this software without specific prior written permission.                    *
 *                                                                                *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"    *
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE      *
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE *
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE   *
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL     *
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR     *
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER     *
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  *
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE  *
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.           *
 *                                                                                *
 **********************************************************************************/


#include "Driver.h"
#include "connection.tmh"


//
// Creates & allocates new connection object and inserts it into connection list
// 
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
ClientConnections_CreateAndInsert(
    _In_ PBTHPS3_SERVER_CONTEXT Context,
    _In_ BTH_ADDR RemoteAddress,
    _In_ PFN_WDF_OBJECT_CONTEXT_CLEANUP CleanupCallback,
    _Out_ PBTHPS3_CLIENT_CONNECTION *ClientConnection
)
{
    NTSTATUS                    status;
    WDF_OBJECT_ATTRIBUTES       attributes;
    WDFOBJECT                   connectionObject = NULL;
    PBTHPS3_CLIENT_CONNECTION   connectionCtx = NULL;

    FuncEntry(TRACE_CONNECTION);

    do
    {
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, BTHPS3_CLIENT_CONNECTION);
        attributes.ParentObject = Context->Header.Device;
        attributes.EvtCleanupCallback = CleanupCallback;
        attributes.ExecutionLevel = WdfExecutionLevelPassive;

        //
        // Create piggyback object carrying the context
        // 
        if (!NT_SUCCESS(status = WdfObjectCreate(
            &attributes,
            &connectionObject
        ))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfObjectCreate for connection object failed with status %!STATUS!",
                status
            );

            break;
        }

        //
        // Piggyback object is parent
        // 
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = connectionObject;

        connectionCtx = GetClientConnection(connectionObject);
        connectionCtx->DevCtxHdr = &Context->Header;

        //
        // Initialize HidControlChannel properties
        // 

        if (!NT_SUCCESS(status = WdfRequestCreate(
            &attributes,
            connectionCtx->DevCtxHdr->IoTarget,
            &connectionCtx->HidControlChannel.ConnectDisconnectRequest
        ))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfRequestCreate for HidControlChannel failed with status %!STATUS!",
                status
            );

            break;
        }

        //
        // Initialize signaled, will be cleared once a connection is established
        // 
        KeInitializeEvent(&connectionCtx->HidControlChannel.DisconnectEvent,
            NotificationEvent,
            TRUE
        );

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = connectionObject;

        if (!NT_SUCCESS(status = WdfSpinLockCreate(
            &attributes,
            &connectionCtx->HidControlChannel.ConnectionStateLock
        ))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfSpinLockCreate for HidControlChannel failed with status %!STATUS!",
                status
            );

            break;
        }

        connectionCtx->HidControlChannel.ConnectionState = ConnectionStateInitialized;

        //
        // Initialize HidInterruptChannel properties
        // 

        if (!NT_SUCCESS(status = WdfRequestCreate(
            &attributes,
            connectionCtx->DevCtxHdr->IoTarget,
            &connectionCtx->HidInterruptChannel.ConnectDisconnectRequest
        ))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfRequestCreate for HidInterruptChannel failed with status %!STATUS!",
                status
            );

            break;
        }

        //
        // Initialize signaled, will be cleared once a connection is established
        // 
        KeInitializeEvent(&connectionCtx->HidInterruptChannel.DisconnectEvent,
            NotificationEvent,
            TRUE
        );

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = connectionObject;

        if (!NT_SUCCESS(status = WdfSpinLockCreate(
            &attributes,
            &connectionCtx->HidInterruptChannel.ConnectionStateLock
        ))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfSpinLockCreate for HidInterruptChannel failed with status %!STATUS!",
                status
            );

            break;
        }

        connectionCtx->HidInterruptChannel.ConnectionState = ConnectionStateInitialized;

        //
        // Insert initialized connection list in connection collection
        // 
        if (!NT_SUCCESS(status = WdfCollectionAdd(Context->Header.Clients, connectionObject))) {
            TraceError(
                TRACE_CONNECTION,
                "WdfCollectionAdd for connection object failed with status %!STATUS!",
                status
            );

            break;
        }

        //
        // This is our "primary key"
        // 
        connectionCtx->RemoteAddress = RemoteAddress;

    	//
    	// Store reported remote name to assign to property later
    	// 
        connectionCtx->RemoteName.MaximumLength = BTH_MAX_NAME_SIZE * sizeof(WCHAR);
        connectionCtx->RemoteName.Buffer = ExAllocatePoolWithTag(
            NonPagedPoolNx,
            BTH_MAX_NAME_SIZE * sizeof(WCHAR),
            BTHPS_POOL_TAG
        );

        //
        // Pass back valid pointer
        // 
        *ClientConnection = connectionCtx;

        status = STATUS_SUCCESS;
    	
    } while (FALSE);

    if (!NT_SUCCESS(status) && connectionObject)
    {
        WdfObjectDelete(connectionObject);
    }

    FuncExit(TRACE_CONNECTION, "status=%!STATUS!", status);
	
    return status;
}

//
// Removes supplied client connection from connection list and frees its resources
// 
VOID
ClientConnections_RemoveAndDestroy(
    _In_ PBTHPS3_DEVICE_CONTEXT_HEADER Context,
    _In_ PBTHPS3_CLIENT_CONNECTION ClientConnection
)
{
    ULONG itemCount;
    ULONG index;
    WDFOBJECT item, currentItem;
	
    FuncEntryArguments(
        TRACE_CONNECTION, 
        "ClientConnection=0x%p",
        ClientConnection
    );	
    
    WdfSpinLockAcquire(Context->ClientsLock);
    
    item = WdfObjectContextGetObject(ClientConnection);
    itemCount = WdfCollectionGetCount(Context->Clients);

    for (index = 0; index < itemCount; index++)
    {
        currentItem = WdfCollectionGetItem(Context->Clients, index);

        if (currentItem == item)
        {
            TraceVerbose(
                TRACE_CONNECTION,
                "Found desired connection item in connection list"
            );

            WdfCollectionRemoveItem(Context->Clients, index);
            WdfObjectDelete(item);
            break;
        }
    }

    WdfSpinLockRelease(Context->ClientsLock);

    FuncExitNoReturn(TRACE_CONNECTION);
}

