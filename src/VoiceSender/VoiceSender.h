#pragma once

#include <cstdint>

class IVEngineServer2;
class INetworkMessages;

namespace BotController
{
    namespace VoiceSender
    {
        // Store engine interfaces used to allocate and send voice messages
        void SetInterfaces(IVEngineServer2 *engine, INetworkMessages *networkMessages);

        // Return true when voice net messages can be sent
        bool IsAvailable();

        // Return 0 when ready, otherwise a negative setup error code
        int GetStatus();

        // Send one encoded voice frame to a recipient player slot
        int SendVoiceFrame(
            int recipientSlot,
            int senderClient,
            uint64_t senderXuid,
            const uint8_t *audio,
            int audioBytes,
            int sampleRate,
            float voiceLevel,
            int sequenceBytes,
            int sectionNumber,
            int uncompressedSampleOffset,
            uint32_t numPackets,
            const uint32_t *packetOffsets,
            int packetOffsetCount,
            int tick,
            int audibleMask);
    }
}
