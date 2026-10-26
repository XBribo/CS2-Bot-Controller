#pragma once

#include <cstdint>

class IVEngineServer2;
class INetworkMessages;

namespace cs2bc {
namespace voice_sender {
// Stores voice sender interfaces.
void SetInterfaces(IVEngineServer2* engine, INetworkMessages* networkMessages);

// Checks voice availability.
bool IsAvailable();

// Returns 0 when ready; negative values report setup errors.
int GetStatus();

// Sends an encoded voice frame to the recipient slot.
int SendVoiceFrame(int recipientSlot,
                   int senderClient,
                   uint64_t senderXuid,
                   const uint8_t* audio,
                   int audioBytes,
                   int sampleRate,
                   float voiceLevel,
                   int sequenceBytes,
                   int sectionNumber,
                   int uncompressedSampleOffset,
                   uint32_t numPackets,
                   const uint32_t* packetOffsets,
                   int packetOffsetCount,
                   int tick,
                   int audibleMask);
} // namespace voice_sender
} // namespace cs2bc
