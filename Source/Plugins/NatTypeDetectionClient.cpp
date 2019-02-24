/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  Copyright (c) 2016-2018, TES3MP Team
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant 
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "NativeFeatureIncludes.h"
#if _CRABNET_SUPPORT_NatTypeDetectionClient==1

#include "NatTypeDetectionClient.h"
#include "RakNetSmartPtr.h"
#include "BitStream.h"
#include "SocketIncludes.h"
#include "RakString.h"
#include "RakPeerInterface.h"
#include "MessageIdentifiers.h"
#include "SocketLayer.h"
#include "../Utils/SocketDefines.h"

using namespace CrabNet;

STATIC_FACTORY_DEFINITIONS(NatTypeDetectionClient,NatTypeDetectionClient)

NatTypeDetectionClient::NatTypeDetectionClient()
{
    c2=0;
}
NatTypeDetectionClient::~NatTypeDetectionClient()
{
    if (c2!=0)
    {
        delete c2;
    }
}
void NatTypeDetectionClient::DetectNATType(SystemAddress _serverAddress)
{
    if (IsInProgress())
        return;

    if (c2==0)
    {
        DataStructures::List<RakNetSocket2* > sockets;
        rakPeerInterface->GetSockets(sockets);
        //SystemAddress sockAddr;
        //SocketLayer::GetSystemAddress(sockets[0], &sockAddr);
        char str[64];
        //sockAddr.ToString(false,str);
        sockets[0]->GetBoundAddress().ToString(false,str);
        c2=CreateNonblockingBoundSocket(str
#ifdef __native_client__
            , sockets[0]->chromeInstance
#endif
            ,this
            );
        //c2Port=SocketLayer::GetLocalPort(c2);
    }

#if !defined(__native_client__)
    if (c2->IsBerkleySocket())
        ((RNS2_Berkley*) c2)->CreateRecvPollingThread(0);
#endif

    serverAddress=_serverAddress;

    CrabNet::BitStream bs;
    bs.Write((unsigned char)ID_NAT_TYPE_DETECTION_REQUEST);
    bs.Write(true); // IsRequest
    bs.Write(c2->GetBoundAddress().GetPort());
    rakPeerInterface->Send(&bs,MEDIUM_PRIORITY,RELIABLE,0,serverAddress,false);
}
void NatTypeDetectionClient::OnCompletion(NATTypeDetectionResult result)
{
    Packet *p = AllocatePacketUnified(sizeof(MessageID)+sizeof(unsigned char)*2);
    //printf("Returning nat detection result to the user\n");
    p->data[0]=ID_NAT_TYPE_DETECTION_RESULT;
    p->systemAddress=serverAddress;
    p->systemAddress.systemIndex=(SystemIndex)-1;
    p->guid=rakPeerInterface->GetGuidFromSystemAddress(serverAddress);
    p->data[1]=(unsigned char) result;
    p->wasGeneratedLocally=true;
    rakPeerInterface->PushBackPacket(p, true);

    // Symmetric and port restricted are determined by server, so no need to notify server we are done
    if (result!=NAT_TYPE_PORT_RESTRICTED && result!=NAT_TYPE_SYMMETRIC)
    {
        // Otherwise tell the server we got this message, so it stops sending tests to us
        CrabNet::BitStream bs;
        bs.Write((unsigned char)ID_NAT_TYPE_DETECTION_REQUEST);
        bs.Write(false); // Done
        rakPeerInterface->Send(&bs,HIGH_PRIORITY,RELIABLE,0,serverAddress,false);
    }

    Shutdown();
}
bool NatTypeDetectionClient::IsInProgress(void) const
{
    return serverAddress!=UNASSIGNED_SYSTEM_ADDRESS;
}
void NatTypeDetectionClient::Update(void)
{
    if (IsInProgress())
    {
        RNS2RecvStruct *recvStruct;
        bufferedPacketsMutex.Lock();
        if (bufferedPackets.Size()>0)
            recvStruct=bufferedPackets.Pop();
        else
            recvStruct=0;
        bufferedPacketsMutex.Unlock();
        while (recvStruct)
        {
            if (recvStruct->bytesRead==1 && recvStruct->data[0]==NAT_TYPE_NONE)
            {
                OnCompletion(NAT_TYPE_NONE);
                RakAssert(IsInProgress()==false);
            }
            DeallocRNS2RecvStruct(recvStruct);

            bufferedPacketsMutex.Lock();
            if (bufferedPackets.Size()>0)
                recvStruct=bufferedPackets.Pop();
            else
                recvStruct=0;
            bufferedPacketsMutex.Unlock();
        }
    }
}
PluginReceiveResult NatTypeDetectionClient::OnReceive(Packet *packet)
{
    if (IsInProgress())
    {
        switch (packet->data[0])
        {
        case ID_OUT_OF_BAND_INTERNAL:
            {
                if (packet->length>=3 && packet->data[1]==ID_NAT_TYPE_DETECT)
                {
                    OnCompletion((NATTypeDetectionResult)packet->data[2]);
                    return RR_STOP_PROCESSING_AND_DEALLOCATE;
                }
            }
            break;
        case ID_NAT_TYPE_DETECTION_RESULT:
            if (packet->wasGeneratedLocally==false)
            {
                OnCompletion((NATTypeDetectionResult)packet->data[1]);
                return RR_STOP_PROCESSING_AND_DEALLOCATE;
            }
            else
                break;
        case ID_NAT_TYPE_DETECTION_REQUEST:
            OnTestPortRestricted(packet);
            return RR_STOP_PROCESSING_AND_DEALLOCATE;
        }
    }

    return RR_CONTINUE_PROCESSING;
}
void NatTypeDetectionClient::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason )
{
    (void) lostConnectionReason;
    (void) rakNetGUID;

    if (IsInProgress() && systemAddress==serverAddress)
        Shutdown();
}
void NatTypeDetectionClient::OnRakPeerShutdown(void)
{
    Shutdown();
}
void NatTypeDetectionClient::OnDetach(void)
{
    Shutdown();
}
void NatTypeDetectionClient::OnTestPortRestricted(Packet *packet)
{
    CrabNet::BitStream bsIn(packet->data,packet->length,false);
    bsIn.IgnoreBytes(sizeof(MessageID));
    CrabNet::RakString s3p4StrAddress;
    bsIn.Read(s3p4StrAddress);
    unsigned short s3p4Port;
    bsIn.Read(s3p4Port);

    DataStructures::List<RakNetSocket2* > sockets;
    rakPeerInterface->GetSockets(sockets);
    SystemAddress s3p4Addr = sockets[0]->GetBoundAddress();
    s3p4Addr.FromStringExplicitPort(s3p4StrAddress.C_String(), s3p4Port);

    // Send off the CrabNet socket to the specified address, message is unformatted
    // Server does this twice, so don't have to unduly worry about packetloss
    CrabNet::BitStream bsOut;
    bsOut.Write((MessageID) NAT_TYPE_PORT_RESTRICTED);
    bsOut.Write(rakPeerInterface->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
//    SocketLayer::SendTo_PC( sockets[0], (const char*) bsOut.GetData(), bsOut.GetNumberOfBytesUsed(), s3p4Addr, __FILE__, __LINE__ );

    RNS2_SendParameters bsp;
    bsp.data = (char*) bsOut.GetData();
    bsp.length = bsOut.GetNumberOfBytesUsed();
    bsp.systemAddress=s3p4Addr;
    sockets[0]->Send(&bsp);

}
void NatTypeDetectionClient::Shutdown(void)
{
    serverAddress=UNASSIGNED_SYSTEM_ADDRESS;
    if (c2!=0)
    {
#if !defined(__native_client__)
        if (c2->IsBerkleySocket())
            ((RNS2_Berkley *)c2)->BlockOnStopRecvPollingThread();
#endif

        delete c2;
        c2=0;
    }

    bufferedPacketsMutex.Lock();
    while (bufferedPackets.Size())
        delete bufferedPackets.Pop();
    bufferedPacketsMutex.Unlock();
}

// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void NatTypeDetectionClient::DeallocRNS2RecvStruct(RNS2RecvStruct *s)
{
    delete s;
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
RNS2RecvStruct *NatTypeDetectionClient::AllocRNS2RecvStruct()
{
    return new RNS2RecvStruct;
}
void NatTypeDetectionClient::OnRNS2Recv(RNS2RecvStruct *recvStruct)
{
    bufferedPacketsMutex.Lock();
    bufferedPackets.Push(recvStruct);
    bufferedPacketsMutex.Unlock();
}

#endif // _CRABNET_SUPPORT_*
