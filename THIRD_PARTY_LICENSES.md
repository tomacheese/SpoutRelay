# Third-Party Licenses

This software is distributed with the following third-party components.

---

## FFmpeg

- **License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1+)
- **Source:** <https://ffmpeg.org/>
- **Build:** [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) (LGPL shared variant)

The distribution package includes the following FFmpeg shared libraries:

- `avcodec-62.dll`
- `avformat-62.dll`
- `avutil-60.dll`
- `swscale-9.dll`
- `swresample-6.dll`
- `avfilter-11.dll`
- `avdevice-62.dll`

These DLLs are built **without** GPL-only components (no libx264, libx265, etc.).  
The FFmpeg source code is available at https://ffmpeg.org/download.html.  
A copy of the LGPL-2.1 license is included in the FFmpeg distribution (`LICENSE.txt`).

Under LGPL-2.1, you may use and redistribute these libraries provided you:

1. Preserve the copyright notices and license text.
2. Make the LGPL library source available (or point to the upstream source).
3. Allow users to replace the LGPL library with a modified version.

---

## Spout2 SDK

- **License:** Spout2 SDK License (permissive)
- **Source:** <https://github.com/leadedge/Spout2>

> This file may be used by anyone free of charge but without any warranty.  
> — [Spout2 License](https://github.com/leadedge/Spout2/blob/master/LICENSE)

Spout2 source code is compiled statically into the application.

---

## spdlog

- **License:** MIT
- **Source:** <https://github.com/gabime/spdlog>

---

## nlohmann/json

- **License:** MIT  
- **Source:** <https://github.com/nlohmann/json>

---

## winpthread (mingw-w64)

- **License:** BSD 2-Clause (winpthread) and MIT (mingw-w64 headers)
- **Source:** <https://sourceforge.net/projects/mingw-w64/>

The distribution package includes `libwinpthread-1.dll`, which is the POSIX
threads implementation for Windows provided by the mingw-w64 project.

This DLL is required at runtime as the C++ standard library thread primitives
(`std::thread`, `std::mutex`, etc.) depend on it when built with MinGW-w64.

Copyright (c) 2011, Kaz Kojima and others (winpthread)  
Copyright (c) 2009, 2010 by Mingw-w64 project

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
