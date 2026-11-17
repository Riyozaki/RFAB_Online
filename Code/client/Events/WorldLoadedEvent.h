#pragma once

// Dispatched synchronously on the game thread, after old transport/tasks are fenced, before
// discovery visits the newly loaded world. There is exactly one native TESLoadGameEvent sink.
struct WorldLoadedEvent {};
