
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
 
using namespace std;

#include "logger.hpp"
#include "utils.hpp"

#include "uda.hpp"
#include "mscp_server.hpp"

void* polling_worker(
    void *context)
{
    mscp_server* server = reinterpret_cast<mscp_server*>(context);

    server->Poll();

    return nullptr;
}


mscp_server::mscp_server(
    uda_c *port) :
        device_c(),
        _hostTimeout(0),
        _controllerFlags(0),
        _abort_polling(false),
        _pollState(PollingState::Wait),
        polling_cond(PTHREAD_COND_INITIALIZER),
        polling_mutex(PTHREAD_MUTEX_INITIALIZER),
        _unitOnline(false),
        _credits(INIT_CREDITS) 
{
    // Alias the port pointer.  We do not own the port, merely reference it.
    _port = port;

    _diskBuffer.reset(new uint8_t[_diskBufferSize + 512]);  // 16mb in-memory disk data
                                                            // + 1 block for write protect flag 

    memset(reinterpret_cast<void*>(_diskBuffer.get()), 0x0, _diskBufferSize + 512);

    StartPollingThread();
}


mscp_server::~mscp_server()
{
    AbortPollingThread();
}

void
mscp_server::StartPollingThread(void)
{
    _abort_polling = false;
    _pollState = PollingState::Wait;

    //
    // Initialize the polling thread and start it.
    // It will wait to be woken to do actual work.
    //
    pthread_attr_t attribs;
    pthread_attr_init(&attribs);

    int status = pthread_create(
        &polling_pthread,
        &attribs,
        &polling_worker,
        reinterpret_cast<void*>(this));

    if (status != 0)
    {
        FATAL("Failed to start mscp server thread.  Status 0x%x", status);
    }

    DEBUG("Polling thread created.");
}

void
mscp_server::AbortPollingThread(void)
{
    pthread_mutex_lock(&polling_mutex);
    _abort_polling = true;
    _pollState = PollingState::Wait;
    pthread_cond_signal(&polling_cond);
    pthread_mutex_unlock(&polling_mutex);

    pthread_cancel(polling_pthread);

    uint32_t status = pthread_join(polling_pthread, NULL);

    if (status != 0)
    {
        FATAL("Failed to join polling thread, status 0x%x", status);
    }

    DEBUG("Polling thread aborted.");  
}

void
mscp_server::Poll(void)
{
    timeout_c timer;

    while(!_abort_polling)
    {
        //
        // Wait to be awoken, then pull commands from the command ring
        //
        DEBUG("Sleeping until awoken.");
        pthread_mutex_lock(&polling_mutex);
        while (_pollState == PollingState::Wait)
        {
            pthread_cond_wait(
                &polling_cond,
                &polling_mutex);
        }

        // Shouldn't happen but if it does we just return to the top.
        if (_pollState == PollingState::InitRun)
        {
           _pollState = PollingState::Run;
        }

        pthread_mutex_unlock(&polling_mutex);

        DEBUG("The sleeper awakes.");

        if (_abort_polling)
        {
            break;
        }

        //
        // Pull commands from the ring until the ring is empty, at which
        // point we sleep until awoken again.
        //
        while(!_abort_polling && _pollState == PollingState::Run)
        {
            shared_ptr<Message> message(_port->GetNextCommand());

            if (nullptr == message)
            {
                DEBUG("Empty command ring; sleeping.");
                break;
            }
  
            DEBUG("Message received.");

            //
            // Handle the message.  We dispatch on opcodes to the
            // appropriate methods.  These methods modify the message
            // object in place; this message object is then posted back
            // to the response ring.
            //
            ControlMessageHeader* header = 
                reinterpret_cast<ControlMessageHeader*>(message->Message);

            uint16_t *cmdbuf = reinterpret_cast<uint16_t*>(message->Message);

            DEBUG("Message opcode 0x%x rsvd 0x%x mod 0x%x", 
                header->Word3.Command.Opcode,
                header->Word3.Command.Reserved,
                header->Word3.Command.Modifiers);

            /*
            for(int i=0;i<8;i++)
            {
                INFO("o%o", cmdbuf[i]);
            }
            */

            uint32_t cmdStatus = 0;

            switch (header->Word3.Command.Opcode)
            {
                case Opcodes::GET_UNIT_STATUS:
                    cmdStatus = GetUnitStatus(message, header->UnitNumber, header->Word3.Command.Modifiers);
                    break;

                case Opcodes::ONLINE:
                    cmdStatus = Online(message, header->UnitNumber, header->Word3.Command.Modifiers);
                    break;

                case Opcodes::SET_CONTROLLER_CHARACTERISTICS:
                    cmdStatus = SetControllerCharacteristics(message);     
                    break;

                case Opcodes::SET_UNIT_CHARACTERISTICS:
                    cmdStatus = SetUnitCharacteristics(message, header->UnitNumber, header->Word3.Command.Modifiers);
                    break;

                case Opcodes::READ:
                    cmdStatus = Read(message, header->UnitNumber, header->Word3.Command.Modifiers);
                    break;

                case Opcodes::WRITE:
                    cmdStatus = Write(message, header->UnitNumber, header->Word3.Command.Modifiers);
                    break;

                default:
                    FATAL("Unimplemented MSCP command 0x%x", header->Word3.Command.Opcode);
                    break;
            }

            DEBUG("cmd 0x%x st 0x%x fl 0x%x", cmdStatus, GET_STATUS(cmdStatus), GET_FLAGS(cmdStatus));

            //
            // Set the endcode and status bits
            //
            header->Word3.End.Status = GET_STATUS(cmdStatus);
            header->Word3.End.Flags = GET_FLAGS(cmdStatus);

            // Set the End code properly -- for an Invalid Command response
            // this is just the End code, for all others it's the End code
            // or'd with the opcode.
            if ((GET_STATUS(cmdStatus) & 0x1f) == Status::INVALID_COMMAND)
            {
                 // Just the END code, no opcode
                 header->Word3.End.Endcode = Endcodes::END;
            }
            else
            {
                 header->Word3.End.Endcode |= Endcodes::END;
            }

            //
            // TODO: credits, etc.
            //
            if (message->Word1.Info.MessageType == MessageTypes::Sequential &&
                header->Word3.End.Endcode & Endcodes::END)
            {
                //
                // We steal the hack from simh:
                // The controller gives all of its credits to the host,
                // thereafter it supplies one credit for every response
                // packet sent.
                //
                // Max 14 credits, also C++ is flaming garbage, thanks for replacing "min"
                // with something so incredibly annoying to use.
                // 
                uint32_t grantedCredits = min(_credits, static_cast<uint32_t>(MAX_CREDITS));
                _credits -= grantedCredits;
                message->Word1.Info.Credits = grantedCredits + 1;
            }

            //
            // Post the response to the port's response ring.
            //
            // TODO: is the retry approach appropriate or necessary?
            for (int retry=0;retry<10;retry++)
            {
                if(_port->PostResponse(message))
                {
                    break;
                }
                timer.wait_us(200);
            }

            // Hack: give interrupts time to settle before doing another transfer.
            timer.wait_us(250); 
 
            //
            // Go around and pick up the next one.
            //
        }

        DEBUG("MSCP Polling thread going back to sleep.");
      
        pthread_mutex_lock(&polling_mutex); 
        if (_pollState == PollingState::InitRestart)
        {
            // Signal the Reset call that we're done so it can return
            // and release the Host.
            _pollState = PollingState::Wait;
            pthread_cond_signal(&polling_cond);
        }
        else if (_pollState == PollingState::InitRun)
        {
            _pollState = PollingState::Run;
        }
        else
        { 
            _pollState = PollingState::Wait;
        }
        pthread_mutex_unlock(&polling_mutex);
        
    }
    DEBUG("MSCP Polling thread exiting."); 
}

uint32_t
mscp_server::GetUnitStatus(
    shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct GetUnitStatusResponseParameters
    {
        uint16_t UnitFlags;
        uint16_t MultiUnitCode;
        uint32_t Reserved0;
        uint64_t UnitIdentifier;
        uint32_t MediaTypeIdentifier;
        uint16_t Reserved1;
        uint16_t ShadowUnit;
        uint16_t GroupSize;
        uint16_t TrackSize;
        uint16_t Reserved2;
        uint16_t CylinderSize;
        uint32_t RCTStuff;
    };
    #pragma pack(pop)

    if (unitNumber != 0)
    {
        return STATUS(Status::UNIT_OFFLINE, 3);  // unknown -- todo move to enum
    } 

    INFO("gusrp size %d", sizeof(GetUnitStatusResponseParameters));

    // Adjust message length for response
    message->MessageLength = sizeof(GetUnitStatusResponseParameters) +
        HEADER_SIZE;

    GetUnitStatusResponseParameters* params = 
        reinterpret_cast<GetUnitStatusResponseParameters*>(
            GetParameterPointer(message));

    params->UnitFlags = 0;  // TODO: 0 for now, which is sane.
    params->MultiUnitCode = 0; // Controller dependent, we don't support multi-unit drives.
    params->UnitIdentifier = UNIT_ID;    // Unit #0 always for now
    params->MediaTypeIdentifier = MEDIA_ID_RA80;   // RA80 always for now
    params->ShadowUnit = unitNumber;  // Always equal to unit number
    
    //
    // For group, and cylinder size we return 0 -- this is appropriate for the
    // underlying storage (disk image on flash) since there are no physical tracks
    // or cylinders to speak of (no seek times, etc.)
    //
    params->TrackSize = 1;  // one block per track, per aa-l619a-tk.
    params->GroupSize = 0;
    params->CylinderSize = 0;

    //
    // Since we do no bad block replacement (no bad blocks possible in a disk image file)
    // the RCT size is one block as required for the volume write protect information.
    // There are no replacement blocks, and no duplicate copies of
    // the RCT are present.
    //
    params->RCTStuff = 0x01000001;
    
    if (_unitOnline)
    {
        return STATUS(Status::SUCCESS, 0);
    }
    else
    {
        return STATUS(Status::UNIT_AVAILABLE, 0);
    } 
}

uint32_t
mscp_server::Online(
    shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct OnlineParameters
    {
        uint16_t UnitFlags alignas(2);
        uint16_t Reserved0 alignas(2);
        uint32_t Reserved1;
        uint32_t Reserved2;
        uint32_t Reserved3;
        uint32_t DeviceParameters;
        uint32_t Reserved4;
    };
    #pragma pack(pop)

    //
    // TODO: Right now, ignoring all incoming parameters.
    // With the exception of write-protection none of them really
    // apply.
    // We still need to flag errors if someone tries to set
    // host-settable flags we can't support.
    //

    // TODO: "The ONLINE command performs a SET UNIT CHARACTERISTICS
    // operation after bringing a unit 'Unit-Online'"
    // This code could be refactored w/th S_U_C handler.
    //
    #pragma pack(push,1)
    struct OnlineResponseParameters
    {
        uint16_t UnitFlags alignas(2);
        uint16_t MultiUnitCode alignas(2);
        uint32_t Reserved0;
        uint64_t UnitIdentifier;
        uint32_t MediaTypeIdentifier;
        uint32_t Reserved1;
        uint32_t UnitSize;
        uint32_t VolumeSerialNumber;
    };
    #pragma pack(pop)

    if (unitNumber != 0)
    {
        return STATUS(Status::UNIT_OFFLINE, 3); // unknown -- move to enum
    }
    
    _unitOnline = true;

    // Adjust message length for response
    message->MessageLength = sizeof(OnlineResponseParameters) + 
        HEADER_SIZE;

    OnlineResponseParameters* params =
        reinterpret_cast<OnlineResponseParameters*>(
            GetParameterPointer(message));

    params->UnitFlags = 0;  // TODO: 0 for now, which is sane.
    params->MultiUnitCode = 0; // Controller dependent, we don't support multi-unit drives.
    params->UnitIdentifier = UNIT_ID;  // Unit #0 always for now
    params->MediaTypeIdentifier = MEDIA_ID_RA80;   // RA80 always for now
    params->UnitSize = _diskBufferSize / 512;
    params->VolumeSerialNumber = 0;  // We report no serial
       
    return STATUS(Status::SUCCESS, 0);  // TODO: subcode "Already Online" 
}

uint32_t
mscp_server::SetControllerCharacteristics(
    shared_ptr<Message> message)
{
    #pragma pack(push,1)
    struct SetControllerCharacteristicsParameters
    {
        uint16_t ControllerFlags;
        uint16_t MSCPVersion;
        uint16_t Reserved;
        uint16_t HostTimeout;
        uint64_t TimeAndDate;
    };
    #pragma pack(pop)
 
    SetControllerCharacteristicsParameters* params =
        reinterpret_cast<SetControllerCharacteristicsParameters*>(
            GetParameterPointer(message));

    //
    // Check the version, if non-zero we must return an Invalid Command
    // end message.
    //
    if (params->MSCPVersion != 0)
    {
        return STATUS(Status::INVALID_COMMAND, 0); // TODO: set sub-status
    }  
    else
    {
        _hostTimeout = params->HostTimeout;
        _controllerFlags = params->ControllerFlags; 

        // At this time we ignore the time and date entirely.
   
        // Prepare the response message 
        params->ControllerFlags = _controllerFlags & 0xfe;  // Mask off 576 byte sectors bit.
                                                            // it's read-only and we're a 512
                                                            // byte sector shop here. 
        params->HostTimeout = 0xff;   // Controller timeout: return the max value.
        params->TimeAndDate = _port->GetControllerIdentifier();  // Controller ID

        return STATUS(Status::SUCCESS, 0);
    }
     
}

uint32_t
mscp_server::SetUnitCharacteristics(
    shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct SetUnitCharacteristicsParameters
    {
        uint16_t UnitFlags;
        uint16_t Reserved0;
        uint32_t Reserved1;
        uint64_t Reserved2;
        uint32_t DeviceDependent;
        uint16_t Reserved3;
        uint16_t Reserved4;
    };
    #pragma pack(pop)

    // TODO: handle Set Write Protect modifier

    // Check unit
    if (unitNumber != 0)
    {
        return STATUS(Status::UNIT_OFFLINE, 0);
    }

    // TODO: mostly same as Online command: should share logic.
    #pragma pack(push,1)
    struct SetUnitCharacteristicsResponseParameters
    {
        uint16_t UnitFlags;
        uint16_t MultiUnitCode;
        uint32_t Reserved0;
        uint64_t UnitIdentifier;
        uint32_t MediaTypeIdentifier;
        uint32_t Reserved1;
        uint16_t ShadowUnit;
        uint32_t UnitSize;
        uint32_t VolumeSerialNumber;
    };
    #pragma pack(pop)

    // Adjust message length for response
    message->MessageLength = sizeof(SetUnitCharacteristicsResponseParameters) +
        HEADER_SIZE;

    SetUnitCharacteristicsResponseParameters* params =
        reinterpret_cast<SetUnitCharacteristicsResponseParameters*>(
            GetParameterPointer(message));

    params->UnitFlags = 0;  // TODO: 0 for now, which is sane.
    params->MultiUnitCode = 0; // Controller dependent, we don't support multi-unit drives.
    params->UnitIdentifier = UNIT_ID;  // Unit #0 always for now
    params->MediaTypeIdentifier = MEDIA_ID_RA80;   // RA80 always for now
    params->UnitSize = _diskBufferSize / 512;
    params->VolumeSerialNumber = 0;  // We report no serial

    return STATUS(Status::SUCCESS, 0); 
}


uint32_t
mscp_server::Read(
    shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct ReadParameters
    {
        uint32_t ByteCount;
        uint32_t BufferPhysicalAddress;  // upper 8 bits are channel address for VAXen
        uint32_t Unused0;
        uint32_t Unused1;
        uint32_t LBN;
    };
    #pragma pack(pop)

    ReadParameters* params =
        reinterpret_cast<ReadParameters*>(GetParameterPointer(message));

    INFO("MSCP READ unit %d pa o%o count %d lbn %d",
        unitNumber,
        params->BufferPhysicalAddress & 0x00ffffff,
        params->ByteCount,
        params->LBN);

    // Check unit
    if (unitNumber != 0)
    {
        return STATUS(Status::UNIT_OFFLINE, 0);
    }

    // TODO: Need to rectify reads/writes to RCT area more cleanly
    // and enforce block size of 512 for RCT area.

    // Check LBN and byte count
    if (params->LBN >= (_diskBufferSize + 512) / 512) 
    {
        return STATUS(Status::INVALID_COMMAND + (0x1c << 8), 0); // TODO: set sub-code
    }

    if (params->ByteCount > (((_diskBufferSize + 512) / 512) - params->LBN) * 512)
    {
        return STATUS(Status::INVALID_COMMAND + (0xc << 8), 0); // TODO: as above
    }

    //
    // OK: do the transfer to memory
    //
    _port->DMAWrite(
        params->BufferPhysicalAddress & 0x00ffffff,
        params->ByteCount,
        _diskBuffer.get() + params->LBN * 512);


    // Set parameters for response.
    // We leave ByteCount as is (for now anyway)
    // And set First Bad Block to 0.  (This is unnecessary since we're
    // not reporting a bad block, but we're doing it for completeness.)
    params->LBN = 0;

    return STATUS(Status::SUCCESS,0);
}

uint32_t
mscp_server::Write(
    shared_ptr<Message> message,
    uint16_t unitNumber,
    uint16_t modifiers)
{
    #pragma pack(push,1)
    struct WriteParameters
    {
        uint32_t ByteCount;
        uint32_t BufferPhysicalAddress;  // upper 8 bits are channel address for VAXen
        uint32_t Unused0;
        uint32_t Unused1;
        uint32_t LBN;
    };
    #pragma pack(pop)

    // TODO: Factor this code out (shared w/Read)
    WriteParameters* params =
        reinterpret_cast<WriteParameters*>(GetParameterPointer(message));

    INFO("MSCP WRITE unit %d pa o%o count %d lbn %d",
        unitNumber,
        params->BufferPhysicalAddress & 0x00ffffff,
        params->ByteCount,
        params->LBN);

    // Check unit
    if (unitNumber != 0)
    {
        return STATUS(Status::UNIT_OFFLINE, 0);
    }

    // Check LBN
    if (params->LBN > (_diskBufferSize + 512) / 512)
    {
        return STATUS(Status::INVALID_COMMAND + (0x1c << 8), 0); // TODO: set sub-code
    }

    // Check byte count 
    if (params->ByteCount > (((_diskBufferSize + 512) / 512) - params->LBN) * 512)
    {
        return STATUS(Status::INVALID_COMMAND + (0x0c << 8), 0); // TODO: as above
    }

    //
    // OK: do the transfer from the PDP-11 to a buffer
    //
    unique_ptr<uint8_t> buffer(_port->DMARead(
        params->BufferPhysicalAddress & 0x00ffffff,
        params->ByteCount));

    // Copy the buffer to our in-memory disk buffer
    memcpy(_diskBuffer.get() + params->LBN * 512, buffer.get(), params->ByteCount);

    // Set parameters for response.
    // We leave ByteCount as is (for now anyway)
    // And set First Bad Block to 0.  (This is unnecessary since we're
    // not reporting a bad block, but we're doing it for completeness.)
    params->LBN = 0;

    return STATUS(Status::SUCCESS,0); 
}

uint8_t*
mscp_server::GetParameterPointer(
    shared_ptr<Message> message)
{
    return reinterpret_cast<ControlMessageHeader*>(message->Message)->Parameters;
}

void 
mscp_server::Reset(void)
{
    DEBUG("Aborting polling due to reset.");

    pthread_mutex_lock(&polling_mutex);
    if (_pollState != PollingState::Wait)
    {
        _pollState = PollingState::InitRestart;

        while (_pollState != PollingState::Wait)
        {
            pthread_cond_wait(
                &polling_cond,
                &polling_mutex);
        }
    }  
    pthread_mutex_unlock(&polling_mutex);

    _credits = INIT_CREDITS;
}


void 
mscp_server::InitPolling(void)
{
    //
    // Wake the polling thread if not already awoken.
    //
    pthread_mutex_lock(&polling_mutex);
    if (true) //!_continue_polling)
    {
        DEBUG("Waking polling thread.");
        _pollState = PollingState::InitRun;
       	pthread_cond_signal(&polling_cond);
    }
    else
    {
        DEBUG("Polling already active.");
    }
    pthread_mutex_unlock(&polling_mutex);
}

