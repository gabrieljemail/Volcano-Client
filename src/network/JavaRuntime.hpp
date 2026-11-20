#pragma once
#ifndef VOLCANO_JAVA_RUNTIME_H
#define VOLCANO_JAVA_RUNTIME_H

#include <cstdint>
#include <functional>
#include <string>

namespace Volcano::JavaRuntime {

// The Java the singleplayer servers run on, and where it is kept. Minecraft 26.1 needs Java 25.
constexpr int MAJOR_VERSION = 25;
constexpr const char* DIRECTORY = "java";
constexpr const char* EXECUTABLE = "java/bin/java.exe"; // The default Singleplayer.JavaPath.

// Whether `javaPath` is the managed install this module downloads (so it is ours to fetch when it is missing).
bool IsManagedPath(const std::string& javaPath);

// Whether the managed Java is on disk.
bool Installed();

// Downloads Eclipse Temurin's JRE for MAJOR_VERSION from Adoptium (checked against the published SHA-256) and unpacks
// it into DIRECTORY. Blocks, so call it from a worker thread. Returns false with the reason in `error`.
bool Download(const std::function<void(uint64_t received, uint64_t total)>& progress, std::string& error);

} // namespace Volcano::JavaRuntime

#endif
