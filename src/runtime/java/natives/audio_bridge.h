#pragma once
#include <jni.h>
namespace venture::java {
void register_audio_tracks(JNIEnv*);
void shutdown_audio_tracks();
}
