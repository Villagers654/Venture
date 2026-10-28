// Guest LP64 OpenSL ES 1.0.1 / Android ABI. Field order and UUIDs from
// AOSP frameworks/wilhelm, android-12.1.0_r1, include/SLES/OpenSLES*.h
// and src/OpenSLES_IID.cpp. These are guest addresses, never host function
// pointers.
#pragma once
#include <cstddef>
#include <cstdint>

#include "core/guest.h"
namespace venture::hle::opensles::abi {
struct InterfaceId {
  std::uint32_t time_low;
  std::uint16_t time_mid, time_hi_and_version, clock_seq;
  std::uint8_t node[6];
};
static_assert(sizeof(InterfaceId) == 16 && offsetof(InterfaceId, node) == 10);
struct Data {
  GuestAddr locator, format;
};
struct QueueLocator {
  std::uint32_t type, buffers;
};
struct UriLocator { std::uint32_t type,padding; GuestAddr uri; };
struct FdLocator { std::uint32_t type; std::int32_t fd; std::int64_t offset,length; };
struct SeekTable { GuestAddr SetPosition,SetLoop,GetLoop; };
static_assert(sizeof(UriLocator)==16 && sizeof(FdLocator)==24 && sizeof(SeekTable)==24);
struct OutputMixLocator {
  std::uint32_t type, padding;
  GuestAddr object;
};
struct IoLocator {
  std::uint32_t type, device_type, device_id, padding;
  GuestAddr object;
};
struct Pcm {
  std::uint32_t type, channels, rate_millihz, bits, container, mask, endian;
};
struct PcmEx {
  Pcm pcm;
  std::uint32_t representation;
};
struct QueueState {
  std::uint32_t count, index;
};
struct EngineOption {
  std::uint32_t feature, data;
};
static_assert(sizeof(Data) == 16 && sizeof(QueueLocator) == 8);
static_assert(sizeof(OutputMixLocator) == 16 &&
              offsetof(OutputMixLocator, object) == 8);
static_assert(sizeof(IoLocator) == 24 && offsetof(IoLocator, object) == 16);
static_assert(sizeof(Pcm) == 28 && sizeof(PcmEx) == 32 &&
              offsetof(PcmEx, representation) == 28);
static_assert(sizeof(QueueState) == 8);
struct ObjectTable {
  GuestAddr Realize;
  GuestAddr Resume;
  GuestAddr GetState;
  GuestAddr GetInterface;
  GuestAddr RegisterCallback;
  GuestAddr AbortAsyncOperation;
  GuestAddr Destroy;
  GuestAddr SetPriority;
  GuestAddr GetPriority;
  GuestAddr SetLossOfControlInterfaces;
};
static_assert(sizeof(ObjectTable) == 80);
static_assert(offsetof(ObjectTable, Realize) == 0);
static_assert(offsetof(ObjectTable, Resume) == 8);
static_assert(offsetof(ObjectTable, GetState) == 16);
static_assert(offsetof(ObjectTable, GetInterface) == 24);
static_assert(offsetof(ObjectTable, RegisterCallback) == 32);
static_assert(offsetof(ObjectTable, AbortAsyncOperation) == 40);
static_assert(offsetof(ObjectTable, Destroy) == 48);
static_assert(offsetof(ObjectTable, SetPriority) == 56);
static_assert(offsetof(ObjectTable, GetPriority) == 64);
static_assert(offsetof(ObjectTable, SetLossOfControlInterfaces) == 72);
struct EngineTable {
  GuestAddr CreateLEDDevice;
  GuestAddr CreateVibraDevice;
  GuestAddr CreateAudioPlayer;
  GuestAddr CreateAudioRecorder;
  GuestAddr CreateMidiPlayer;
  GuestAddr CreateListener;
  GuestAddr Create3DGroup;
  GuestAddr CreateOutputMix;
  GuestAddr CreateMetadataExtractor;
  GuestAddr CreateExtensionObject;
  GuestAddr QueryNumSupportedInterfaces;
  GuestAddr QuerySupportedInterfaces;
  GuestAddr QueryNumSupportedExtensions;
  GuestAddr QuerySupportedExtension;
  GuestAddr IsExtensionSupported;
};
static_assert(sizeof(EngineTable) == 120);
static_assert(offsetof(EngineTable, CreateLEDDevice) == 0);
static_assert(offsetof(EngineTable, CreateVibraDevice) == 8);
static_assert(offsetof(EngineTable, CreateAudioPlayer) == 16);
static_assert(offsetof(EngineTable, CreateAudioRecorder) == 24);
static_assert(offsetof(EngineTable, CreateMidiPlayer) == 32);
static_assert(offsetof(EngineTable, CreateListener) == 40);
static_assert(offsetof(EngineTable, Create3DGroup) == 48);
static_assert(offsetof(EngineTable, CreateOutputMix) == 56);
static_assert(offsetof(EngineTable, CreateMetadataExtractor) == 64);
static_assert(offsetof(EngineTable, CreateExtensionObject) == 72);
static_assert(offsetof(EngineTable, QueryNumSupportedInterfaces) == 80);
static_assert(offsetof(EngineTable, QuerySupportedInterfaces) == 88);
static_assert(offsetof(EngineTable, QueryNumSupportedExtensions) == 96);
static_assert(offsetof(EngineTable, QuerySupportedExtension) == 104);
static_assert(offsetof(EngineTable, IsExtensionSupported) == 112);
struct PlayTable {
  GuestAddr SetPlayState;
  GuestAddr GetPlayState;
  GuestAddr GetDuration;
  GuestAddr GetPosition;
  GuestAddr RegisterCallback;
  GuestAddr SetCallbackEventsMask;
  GuestAddr GetCallbackEventsMask;
  GuestAddr SetMarkerPosition;
  GuestAddr ClearMarkerPosition;
  GuestAddr GetMarkerPosition;
  GuestAddr SetPositionUpdatePeriod;
  GuestAddr GetPositionUpdatePeriod;
};
static_assert(sizeof(PlayTable) == 96);
static_assert(offsetof(PlayTable, SetPlayState) == 0);
static_assert(offsetof(PlayTable, GetPlayState) == 8);
static_assert(offsetof(PlayTable, GetDuration) == 16);
static_assert(offsetof(PlayTable, GetPosition) == 24);
static_assert(offsetof(PlayTable, RegisterCallback) == 32);
static_assert(offsetof(PlayTable, SetCallbackEventsMask) == 40);
static_assert(offsetof(PlayTable, GetCallbackEventsMask) == 48);
static_assert(offsetof(PlayTable, SetMarkerPosition) == 56);
static_assert(offsetof(PlayTable, ClearMarkerPosition) == 64);
static_assert(offsetof(PlayTable, GetMarkerPosition) == 72);
static_assert(offsetof(PlayTable, SetPositionUpdatePeriod) == 80);
static_assert(offsetof(PlayTable, GetPositionUpdatePeriod) == 88);
struct RecordTable {
  GuestAddr SetRecordState;
  GuestAddr GetRecordState;
  GuestAddr SetDurationLimit;
  GuestAddr GetPosition;
  GuestAddr RegisterCallback;
  GuestAddr SetCallbackEventsMask;
  GuestAddr GetCallbackEventsMask;
  GuestAddr SetMarkerPosition;
  GuestAddr ClearMarkerPosition;
  GuestAddr GetMarkerPosition;
  GuestAddr SetPositionUpdatePeriod;
  GuestAddr GetPositionUpdatePeriod;
};
static_assert(sizeof(RecordTable) == 96);
static_assert(offsetof(RecordTable, SetRecordState) == 0);
static_assert(offsetof(RecordTable, GetRecordState) == 8);
static_assert(offsetof(RecordTable, SetDurationLimit) == 16);
static_assert(offsetof(RecordTable, GetPosition) == 24);
static_assert(offsetof(RecordTable, RegisterCallback) == 32);
static_assert(offsetof(RecordTable, SetCallbackEventsMask) == 40);
static_assert(offsetof(RecordTable, GetCallbackEventsMask) == 48);
static_assert(offsetof(RecordTable, SetMarkerPosition) == 56);
static_assert(offsetof(RecordTable, ClearMarkerPosition) == 64);
static_assert(offsetof(RecordTable, GetMarkerPosition) == 72);
static_assert(offsetof(RecordTable, SetPositionUpdatePeriod) == 80);
static_assert(offsetof(RecordTable, GetPositionUpdatePeriod) == 88);
struct VolumeTable {
  GuestAddr SetVolumeLevel;
  GuestAddr GetVolumeLevel;
  GuestAddr GetMaxVolumeLevel;
  GuestAddr SetMute;
  GuestAddr GetMute;
  GuestAddr EnableStereoPosition;
  GuestAddr IsEnabledStereoPosition;
  GuestAddr SetStereoPosition;
  GuestAddr GetStereoPosition;
};
static_assert(sizeof(VolumeTable) == 72);
static_assert(offsetof(VolumeTable, SetVolumeLevel) == 0);
static_assert(offsetof(VolumeTable, GetVolumeLevel) == 8);
static_assert(offsetof(VolumeTable, GetMaxVolumeLevel) == 16);
static_assert(offsetof(VolumeTable, SetMute) == 24);
static_assert(offsetof(VolumeTable, GetMute) == 32);
static_assert(offsetof(VolumeTable, EnableStereoPosition) == 40);
static_assert(offsetof(VolumeTable, IsEnabledStereoPosition) == 48);
static_assert(offsetof(VolumeTable, SetStereoPosition) == 56);
static_assert(offsetof(VolumeTable, GetStereoPosition) == 64);
struct BufferQueueTable {
  GuestAddr Enqueue;
  GuestAddr Clear;
  GuestAddr GetState;
  GuestAddr RegisterCallback;
};
static_assert(sizeof(BufferQueueTable) == 32);
static_assert(offsetof(BufferQueueTable, Enqueue) == 0);
static_assert(offsetof(BufferQueueTable, Clear) == 8);
static_assert(offsetof(BufferQueueTable, GetState) == 16);
static_assert(offsetof(BufferQueueTable, RegisterCallback) == 24);
struct AndroidConfigurationTable {
  GuestAddr SetConfiguration;
  GuestAddr GetConfiguration;
  GuestAddr AcquireJavaProxy;
  GuestAddr ReleaseJavaProxy;
};
static_assert(sizeof(AndroidConfigurationTable) == 32);
static_assert(offsetof(AndroidConfigurationTable, SetConfiguration) == 0);
static_assert(offsetof(AndroidConfigurationTable, GetConfiguration) == 8);
static_assert(offsetof(AndroidConfigurationTable, AcquireJavaProxy) == 16);
static_assert(offsetof(AndroidConfigurationTable, ReleaseJavaProxy) == 24);
struct AndroidSimpleBufferQueueTable {
  GuestAddr Enqueue;
  GuestAddr Clear;
  GuestAddr GetState;
  GuestAddr RegisterCallback;
};
static_assert(sizeof(AndroidSimpleBufferQueueTable) == 32);
static_assert(offsetof(AndroidSimpleBufferQueueTable, Enqueue) == 0);
static_assert(offsetof(AndroidSimpleBufferQueueTable, Clear) == 8);
static_assert(offsetof(AndroidSimpleBufferQueueTable, GetState) == 16);
static_assert(offsetof(AndroidSimpleBufferQueueTable, RegisterCallback) == 24);
}  // namespace venture::hle::opensles::abi
