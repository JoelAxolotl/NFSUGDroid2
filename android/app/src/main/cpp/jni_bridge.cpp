// JNI entry points for com.rea.nfsug2.NativeBridge.
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <string>

#include "core/log.h"
#include "platform/input.h"
#include "platform/window.h"
#include "runtime/process.h"

namespace {
std::atomic<bool> g_started{false};

std::string jstr(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(s, c);
    return r;
}
}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_rea_nfsug2_NativeBridge_start(JNIEnv* env, jclass, jstring game,
                                                                   jstring user) {
    if (g_started.exchange(true)) return JNI_TRUE;
    proc::Config cfg{jstr(env, game), jstr(env, user)};
    std::string err;
    if (!proc::boot(cfg, err)) {
        LOGE("boot failed: %s", err.c_str());
        proc::set_status("Boot failed: " + err);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_rea_nfsug2_NativeBridge_surfaceChanged(JNIEnv* env, jclass, jobject surface,
                                                                        jint w, jint h) {
    window::set_surface(ANativeWindow_fromSurface(env, surface), w, h);
}

JNIEXPORT void JNICALL Java_com_rea_nfsug2_NativeBridge_surfaceDestroyed(JNIEnv*, jclass) {
    window::clear_surface();
}

JNIEXPORT void JNICALL Java_com_rea_nfsug2_NativeBridge_setPaused(JNIEnv*, jclass, jboolean p) {
    window::set_paused(p == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_rea_nfsug2_NativeBridge_setPadState(JNIEnv*, jclass, jint source, jint buttons,
                                                                     jfloat lx, jfloat ly, jfloat rx, jfloat ry,
                                                                     jfloat lt, jfloat rt) {
    input::Pad p;
    p.buttons = (uint32_t)buttons;
    p.lx = lx;
    p.ly = ly;
    p.rx = rx;
    p.ry = ry;
    p.lt = lt;
    p.rt = rt;
    input::set_pad(source, p);
}

JNIEXPORT void JNICALL Java_com_rea_nfsug2_NativeBridge_keyEvent(JNIEnv*, jclass, jint dik, jboolean down) {
    input::set_key(dik, down == JNI_TRUE);
}

JNIEXPORT jstring JNICALL Java_com_rea_nfsug2_NativeBridge_status(JNIEnv* env, jclass) {
    return env->NewStringUTF(proc::status().c_str());
}

JNIEXPORT jboolean JNICALL Java_com_rea_nfsug2_NativeBridge_isRunning(JNIEnv*, jclass) {
    // Before boot completes, report running so the activity does not close.
    return (!g_started.load() || proc::running()) ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
