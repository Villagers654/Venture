#pragma once
#include <jni.h>
namespace venture::java {
void register_media_players(JNIEnv*);
void register_surface_textures(JNIEnv*);
void shutdown_media_players();
}
