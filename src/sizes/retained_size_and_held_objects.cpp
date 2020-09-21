// Copyright 2000-2018 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.

#include <vector>
#include "retained_size_and_held_objects.h"
#include "sizes_callbacks.h"
#include "sizes_tags.h"

jint JNICALL countSizeAndRetagHeldObjects(jlong classTag, jlong size, jlong *tagPtr, jint length, void *userData) {
    if (*tagPtr == 0) {
        return JVMTI_ITERATION_CONTINUE;
    }

    Tag *tag = tagToPointer(*tagPtr);
    *tagPtr = 0;
    for (query_size_t i = 0; i < tag->array.getSize(); i++) {
        const TagInfoArray::TagInfo &info = tag->array[i];
        if (isRetained(info.state)) {
            *tagPtr = pointerToTag(&Tag::HeldObjectTag);
            *reinterpret_cast<jlong *>(userData) += size;
        }
    }

    tag->unref();
    return JVMTI_ITERATION_CONTINUE;
}

RetainedSizeAndHeldObjectsAction::RetainedSizeAndHeldObjectsAction(JNIEnv *env, jvmtiEnv *jvmti) : MemoryAgentTimedAction(env, jvmti) {

}

jvmtiError RetainedSizeAndHeldObjectsAction::retagStartObject(jobject object) {
    jlong oldTag;
    jvmtiError err = jvmti->GetTag(object, &oldTag);
    if (!isOk(err)) return err;

    if (isTagWithNewInfo(oldTag)) {
        Tag *tag = Tag::create(0, createState(true, true, false, false));
        err = jvmti->SetTag(object, pointerToTag(tag));
    }

    return err;
}

jvmtiError RetainedSizeAndHeldObjectsAction::tagHeap(jobject object) {
    jvmtiError err = FollowReferences(0, nullptr, nullptr, getTagsWithNewInfo, &finishTime, "find objects with new info");
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    std::vector<std::pair<jobject, jlong>> objectsAndTags;
    debug("collect objects with new info");
    err = getObjectsByTags(jvmti, std::vector<jlong>{pointerToTag(&Tag::TagWithNewInfo)}, objectsAndTags);
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    err = retagStartObject(object);
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    err = FollowReferences(0, nullptr, nullptr, visitReference, &finishTime, "getTag heap");
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    return walkHeapFromObjects(jvmti, objectsAndTags, finishTime);
}

jvmtiError RetainedSizeAndHeldObjectsAction::estimateObjectSize(jobject &object, jlong &retainedSize, std::vector<jobject> &heldObjects) {
    Tag *tag = Tag::create(0, createState(true, true, false, false));
    jvmtiError err = jvmti->SetTag(object, pointerToTag(tag));
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    err = tagHeap(object);
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    retainedSize = 0;
    err = IterateThroughHeap(JVMTI_HEAP_FILTER_UNTAGGED, nullptr, countSizeAndRetagHeldObjects,
                             &retainedSize, "calculate retained size");
    if (!isOk(err)) return err;
    if (shouldStopExecution()) return MEMORY_AGENT_TIMEOUT_ERROR;

    if (sizesTagBalance != 0) {
        fatal("MEMORY LEAK FOUND!");
    }

    debug("collect held objects");
    return getObjectsByTags(jvmti, std::vector<jlong>{pointerToTag(&Tag::HeldObjectTag)}, heldObjects);
}

jobjectArray RetainedSizeAndHeldObjectsAction::createResultObject(jlong retainedSize, const std::vector<jobject> &heldObjects) {
    jint objectsCount = static_cast<jint>(heldObjects.size());
    jclass langObject = env->FindClass("java/lang/Object");
    jobjectArray resultObjects = env->NewObjectArray(objectsCount, langObject, nullptr);

    for (jsize i = 0; i < objectsCount; ++i) {
        env->SetObjectArrayElement(resultObjects, i, heldObjects[i]);
    }

    jobjectArray result = env->NewObjectArray(2, langObject, nullptr);
    env->SetObjectArrayElement(result, 0, toJavaArray(env, retainedSize));
    env->SetObjectArrayElement(result, 1, resultObjects);

    return result;
}

jobjectArray RetainedSizeAndHeldObjectsAction::executeOperation(jobject object) {
    std::vector<jobject> objects;
    objects.push_back(object);
    std::vector<jobject> heldObjects;
    jlong retainedSize;
    jvmtiError err = estimateObjectSize(object, retainedSize, heldObjects);

    if (!isOk(err)) {
        handleError(jvmti, err, "Could not estimate object size");
    }

    return createResultObject(retainedSize, heldObjects);
}

jvmtiError RetainedSizeAndHeldObjectsAction::cleanHeap() {
    return removeAllTagsFromHeap(jvmti, nullptr);
}
