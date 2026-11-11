#pragma once

#include "MeshSource.h"

#include <filesystem>

MeshSource loadObj(const std::filesystem::path& path);
