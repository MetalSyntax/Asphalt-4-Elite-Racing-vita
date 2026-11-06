/**
 * @file hle.h
 * @brief Canonical storage path constants for PS Vita high-level emulation.
 */
#pragma once

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/asphalt4eliteracing/"
#endif

// The .app folder name is load-bearing: ChangeCurrentDirToApplicationDir() (0x5d038) cuts the bundle
// path at its last '/' and appends the hardcoded "/ASPHALT4.app/" to build g_AppPath.
#define BUNDLE_PATH DATA_PATH "ASPHALT4.app/"
#define EXEC_NAME   "ASPHALT4"
#define LOG_PREFIX  "asphalt4eliteracing_"
#define DOCS_PATH   DATA_PATH "saves/"
#define LOGS_PATH   DATA_PATH "logs/"
