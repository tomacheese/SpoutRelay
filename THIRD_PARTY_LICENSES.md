# Third-Party Licenses

This software is distributed with the following third-party components.

---

## FFmpeg

**License:** GNU Lesser General Public License v2.1 or later (LGPL-2.1+)  
**Source:** https://ffmpeg.org/  
**Build:** [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) (LGPL shared variant)

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

**License:** Spout2 SDK License (permissive)  
**Source:** https://github.com/leadedge/Spout2

> This file may be used by anyone free of charge but without any warranty.  
> — [Spout2 License](https://github.com/leadedge/Spout2/blob/master/LICENSE)

Spout2 source code is compiled statically into the application.

---

## spdlog

**License:** MIT  
**Source:** https://github.com/gabime/spdlog

```
The MIT License (MIT)

Copyright (c) 2016 Gabi Melman and spdlog contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## nlohmann/json

**License:** MIT  
**Source:** https://github.com/nlohmann/json

```
MIT License

Copyright (c) 2013-2022 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
