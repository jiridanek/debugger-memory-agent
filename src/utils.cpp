// Copyright 2000-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.

#include <jvmti.h>
#include <sstream>
#include <cstring>
#include <vector>
#include "utils.h"
#include "memory_agent_action.h"
#include "log.h"

const char *getReferenceTypeDescription(jvmtiHeapReferenceKind kind) {
    if (kind == JVMTI_HEAP_REFERENCE_CLASS) return "Reference from an object to its class.";
    if (kind == JVMTI_HEAP_REFERENCE_FIELD)
        return "Reference from an object to the value of one of its instance fields.";
    if (kind == JVMTI_HEAP_REFERENCE_ARRAY_ELEMENT) return "Reference from an array to one of its elements.";
    if (kind == JVMTI_HEAP_REFERENCE_CLASS_LOADER) return "Reference from a class to its class loader.";
    if (kind == JVMTI_HEAP_REFERENCE_SIGNERS) return "Reference from a class to its signers array.";
    if (kind == JVMTI_HEAP_REFERENCE_PROTECTION_DOMAIN) return "Reference from a class to its protection domain.";
    if (kind == JVMTI_HEAP_REFERENCE_INTERFACE)
        return "Reference from a class to one of its interfaces. Note: interfaces are defined via a constant pool "
               "reference, so the referenced interfaces may also be reported with a JVMTI_HEAP_REFERENCE_CONSTANT_"
               "POOL reference kind.";
    if (kind == JVMTI_HEAP_REFERENCE_STATIC_FIELD)
        return "Reference from a class to the value of one of its static fields.";
    if (kind == JVMTI_HEAP_REFERENCE_CONSTANT_POOL)
        return "Reference from a class to a resolved entry in the constant pool.";
    if (kind == JVMTI_HEAP_REFERENCE_SUPERCLASS)
        return "Reference from a class to its superclass. A callback is bot sent if the superclass is "
               "java.lang.Object. Note: loaded classes define superclasses via a constant pool reference, "
               "so the referenced superclass may also be reported with a JVMTI_HEAP_REFERENCE_CONSTANT_POOL "
               "reference kind.";
    if (kind == JVMTI_HEAP_REFERENCE_JNI_GLOBAL) return "Heap root reference: JNI global reference.";
    if (kind == JVMTI_HEAP_REFERENCE_SYSTEM_CLASS) return "Heap root reference: System class.";
    if (kind == JVMTI_HEAP_REFERENCE_MONITOR) return "Heap root reference: monitor.";
    if (kind == JVMTI_HEAP_REFERENCE_STACK_LOCAL) return "Heap root reference: local variable on the stack.";
    if (kind == JVMTI_HEAP_REFERENCE_JNI_LOCAL) return "Heap root reference: JNI local reference.";
    if (kind == JVMTI_HEAP_REFERENCE_THREAD) return "Heap root reference: Thread.";
    if (kind == JVMTI_HEAP_REFERENCE_OTHER) return "Heap root reference: other heap root reference.";
    return "Unknown reference kind";
}

jobjectArray toJavaArray(JNIEnv *env, std::vector<jobject> &objects) {
    auto count = static_cast<jsize>(objects.size());
    jobjectArray res = env->NewObjectArray(count, env->FindClass("java/lang/Object"), nullptr);
    for (auto i = 0; i < count; ++i) {
        env->SetObjectArrayElement(res, i, objects[i]);
    }
    return res;
}

jlongArray toJavaArray(JNIEnv *env, std::vector<jlong> &items) {
    auto count = static_cast<jsize>(items.size());
    jlongArray result = env->NewLongArray(count);
    env->SetLongArrayRegion(result, 0, count, items.data());
    return result;
}

jintArray toJavaArray(JNIEnv *env, std::vector<jint> &items) {
    auto count = static_cast<jsize>(items.size());
    jintArray result = env->NewIntArray(count);
    env->SetIntArrayRegion(result, 0, count, items.data());
    return result;
}

jbooleanArray toJavaArray(JNIEnv *env, std::vector<jboolean> &items) {
    auto count = static_cast<jsize>(items.size());
    jbooleanArray result = env->NewBooleanArray(count);
    env->SetBooleanArrayRegion(result, 0, count, items.data());
    return result;
}

jintArray toJavaArray(JNIEnv *env, jint value) {
    std::vector<jint> vector = {value};
    return toJavaArray(env, vector);
}

jlongArray toJavaArray(JNIEnv *env, jlong value) {
    std::vector<jlong> vector = {value};
    return toJavaArray(env, vector);
}

jobjectArray wrapWithArray(JNIEnv *env, jobject first, jobject second) {
    jobjectArray res = env->NewObjectArray(2, env->FindClass("java/lang/Object"), nullptr);
    env->SetObjectArrayElement(res, 0, first);
    env->SetObjectArrayElement(res, 1, second);
    return res;
}

bool isOk(jvmtiError error) {
    return error == JVMTI_ERROR_NONE;
}

void fromJavaArray(JNIEnv *env, jobjectArray javaArray, std::vector<jobject> &result) {
    auto arrayLength = static_cast<size_t>(env->GetArrayLength(javaArray));
    result.resize(arrayLength);
    for (jsize i = 0; i < arrayLength; ++i) {
        result[i] = env->GetObjectArrayElement(javaArray, i);
    }
}

std::vector<jobject> fromJavaArray(JNIEnv *env, jobjectArray javaArray) {
    std::vector<jobject> result;
    fromJavaArray(env, javaArray, result);
    return result;
}

void handleError(jvmtiEnv *jvmti, jvmtiError err, const char *message) {
    if (!isOk(err) && err != MEMORY_AGENT_INTERRUPTED_ERROR) {
        char *errorName = nullptr;
        const char *name;
        if (jvmti->GetErrorName(err, &errorName) != JVMTI_ERROR_NONE) {
            name = "UNKNOWN";
        } else {
            name = errorName;
        }

        std::stringstream ss;

        ss << "ERROR: JVMTI: " << err << "(" << name << "): " << message << std::endl;
        logger::error(ss.str().data());
    }
}

typedef std::pair<std::set<jlong> *, tagReleasedCallback> iterationInfo;

static jint JNICALL freeObjectCallback(jlong classTag, jlong size, jlong *tagPtr, jint length, void *userData) {
    auto info = reinterpret_cast<iterationInfo *>(userData);
    jlong tagValue = *tagPtr;
    *tagPtr = 0;
    if (info->second && info->first->find(tagValue) == info->first->end()) {
        info->second(tagValue);
    }

    return JVMTI_ITERATION_CONTINUE;
}

jvmtiError removeTagsFromHeap(jvmtiEnv *jvmti, std::set<jlong> &ignoredTags, tagReleasedCallback callback) {
    jvmtiHeapCallbacks cb;
    std::memset(&cb, 0, sizeof(jvmtiHeapCallbacks));
    cb.heap_iteration_callback = freeObjectCallback;
    iterationInfo userData(&ignoredTags, callback);
    logger::debug("remove tags");
    jvmtiError err = jvmti->IterateThroughHeap(JVMTI_HEAP_FILTER_UNTAGGED, nullptr, &cb, &userData);
    logger::debug("tags removed");
    return err;
}

static jvmtiError collectObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &tags, jint &objectsCount, jobject **objects, jlong **objectsTags) {
    auto tagsCount = static_cast<jint>(tags.size());

    logger::debug("call GetObjectsWithTags");
    jvmtiError err = jvmti->GetObjectsWithTags(tagsCount, tags.data(), &objectsCount, objects, objectsTags);
    logger::debug("call GetObjectsWithTags finished");

    return err;
}

static jvmtiError deallocateArrays(jvmtiEnv *jvmti, jobject *objects, jlong *objectsTags) {
    jvmtiError err = jvmti->Deallocate(reinterpret_cast<unsigned char *>(objects));
    if (isOk(err)) {
        err = jvmti->Deallocate(reinterpret_cast<unsigned char *>(objectsTags));
    }

    return err;
}

jvmtiError getObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &tags, std::vector<jobject> &result) {
    jint objectsCount;
    jobject *objects;
    jlong *objectsTags;

    jvmtiError err = collectObjectsByTags(jvmti, tags, objectsCount, &objects, &objectsTags);
    if (!isOk(err)) return err;

    for (int i = 0; i < objectsCount; ++i) {
        result.emplace_back(objects[i]);
    }

    return deallocateArrays(jvmti, objects, objectsTags);
}

jvmtiError getObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &&tags, std::vector<jobject> &result) {
    return getObjectsByTags(jvmti, tags, result);
}

jvmtiError getObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &tags, std::vector<std::pair<jobject, jlong>> &result) {
    jint objectsCount = 0;
    jobject *objects;
    jlong *objectsTags;

    jvmtiError err = collectObjectsByTags(jvmti, tags, objectsCount, &objects, &objectsTags);
    if (!isOk(err)) return err;

    for (int i = 0; i < objectsCount; ++i) {
        result.emplace_back(objects[i], objectsTags[i]);
    }

    return deallocateArrays(jvmti, objects, objectsTags);
}

jvmtiError getObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &&tags, std::vector<std::pair<jobject, jlong>> &result) {
    return getObjectsByTags(jvmti, tags, result);
}

jvmtiError cleanHeapAndGetObjectsByTags(jvmtiEnv *jvmti, std::vector<jlong> &tags,
                                        std::vector<std::pair<jobject, jlong>> &result, tagReleasedCallback callback) {
    std::set<jlong> uniqueTags(tags.begin(), tags.end());
    removeTagsFromHeap(jvmti, uniqueTags, callback);
    tags.assign(uniqueTags.begin(), uniqueTags.end());

    return getObjectsByTags(jvmti, tags, result);
}

jvmtiError removeAllTagsFromHeap(jvmtiEnv *jvmti, tagReleasedCallback callback) {
    std::set<jlong> ignored;
    return removeTagsFromHeap(jvmti, ignored, callback);
}

bool fileExists(const std::string &fileName) {
    return !fileName.empty() && static_cast<bool>(std::ifstream(fileName));
}

std::string jstringTostring(JNIEnv *env, jstring jStr) {
    if (!jStr) {
        return "";
    }

    const char *chars = env->GetStringUTFChars(jStr, nullptr);
    std::string str = std::string(chars);
    env->ReleaseStringUTFChars(jStr, chars);
    return str;
}

jvmtiError tagClassAndItsInheritors(JNIEnv *env, jvmtiEnv *jvmti, jobject classObject, std::function<jlong (jlong)> &&createTag) {
    jclass *classes;
    jint cnt;
    jvmtiError err = jvmti->GetLoadedClasses(&cnt, &classes);
    if (err != JVMTI_ERROR_NONE) return err;

    jclass langClass = env->FindClass("java/lang/Class");
    jmethodID isAssignableFrom = env->GetMethodID(langClass, "isAssignableFrom", "(Ljava/lang/Class;)Z");

    for (int i = 0; i < cnt; i++) {
        if (env->CallBooleanMethod(classObject, isAssignableFrom, classes[i])) {
            jlong oldTag;
            err = jvmti->GetTag(classes[i], &oldTag);
            if (err != JVMTI_ERROR_NONE) return err;

            jlong newTag = createTag(oldTag);
            if (newTag != 0)  {
                err = jvmti->SetTag(classes[i], newTag);
                if (err != JVMTI_ERROR_NONE) return err;
            }
        }
    }

    return err;
}

jmethodID getIsAssignableFromMethod(JNIEnv *env) {
    jclass langClass = env->FindClass("java/lang/Class");
    return env->GetMethodID(langClass, "isAssignableFrom", "(Ljava/lang/Class;)Z");
}

std::string getToString(JNIEnv *env, jobject object) {
    jobject name = env->CallObjectMethod(object, env->GetMethodID(env->FindClass("java/lang/Object"), "toString", "()Ljava/lang/String;"));
    return jstringTostring(env, reinterpret_cast<jstring>(name));
}

ThreadSuspender::ThreadSuspender(jvmtiEnv *jvmti) : jvmti(jvmti) {
    jint threadCnt;
    jthread *threads;
    jvmtiError err = jvmti->GetAllThreads(&threadCnt, &threads);
    if (!isOk(err)) {
        handleError(jvmti, err, "Failed to get all threads");
        return;
    }

    jthread currentThread;
    err = jvmti->GetCurrentThread(&currentThread);
    if (!isOk(err)) {
        handleError(jvmti, err, "Failed to get current thread");
        return;
    }

    jvmtiThreadInfo currentThreadInfo;
    jvmti->GetThreadInfo(currentThread, &currentThreadInfo);
    if (!isOk(err)) {
        handleError(jvmti, err, "Failed to get current thread's info");
        return;
    }

    for (int i = 0; i < threadCnt; i++) {
        jvmtiThreadInfo info;
        jthread thread = threads[i];
        err = jvmti->GetThreadInfo(thread, &info);
        if (!isOk(err)) {
            handleError(jvmti, err, "Failed to get thread's info");
            return;
        }

        if (strcmp(info.name, currentThreadInfo.name) != 0) {
            if (jvmti->SuspendThread(thread) == JVMTI_ERROR_NONE) {
                suspendedThreads.push_back(thread);
            }
        }
    }
}

ThreadSuspender::~ThreadSuspender() {
    for (jthread thread : suspendedThreads) {
        jvmtiError err = jvmti->ResumeThread(thread);
        if (!isOk(err)) {
            handleError(jvmti, err, "Failed to resume thread");
        }
    }
}
