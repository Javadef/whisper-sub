import sys

with open(r"C:\Users\Java\Desktop\sdi3\main.cpp", "r", encoding="utf-8") as f:
    src = f.read()

# Find start: the "ULWord nIn = 0" line
START = '    ULWord nIn = 0, nOut = 0, nDrop = 0;\n'
# Find end: the line just before the shutdown comment block
END   = '\n    // -----------------------------------------------------------------------\n    // 10. Shutdown\n'

i1 = src.index(START)
i2 = src.index(END)

before = src[:i1]
after  = src[i2:]

new_loop = r"""    ULWord nIn = 0, nOut = 0, nDrop = 0;
    ULWord cachedAudioBytes = 0;
    bool   hasFrame = false; // true once vBuf holds at least one processed frame

    // -----------------------------------------------------------------------
    // 9. Main processing loop
    // -----------------------------------------------------------------------
    while (!gQuit)
    {
        // Sample output level BEFORE capture so we know if it is already starving.
        // Starving = < 2 frames queued: output will blank if we do not push now.
        AUTOCIRCULATE_STATUS os;
        device.AutoCirculateGetStatus(NTV2_CHANNEL2, os);
        const bool outStarving = (os.acState == NTV2_AUTOCIRCULATE_RUNNING)
                               && (os.acBufferLevel < 2);

        // Attempt to capture a new frame
        AUTOCIRCULATE_STATUS cs;
        device.AutoCirculateGetStatus(NTV2_CHANNEL1, cs);
        bool newFrame = false;
        if (cs.acState == NTV2_AUTOCIRCULATE_RUNNING && cs.acBufferLevel > 0)
        {
            AUTOCIRCULATE_TRANSFER inXfer;
            inXfer.SetVideoBuffer(reinterpret_cast<ULWord*>(vBuf.data()), fSize);
            inXfer.SetAudioBuffer(reinterpret_cast<ULWord*>(aBuf.data()), kMaxAudioBytes);
            if (device.AutoCirculateTransfer(NTV2_CHANNEL1, inXfer))
            {
                DrawTimestamp(vBuf.data(), stride, 40, 40, 4);
                cachedAudioBytes = inXfer.acAudioTransferSize;
                hasFrame = true;
                newFrame = true;
                nIn++;
            }
        }

        // Push to output:
        //   Normal path  - push on every new captured frame (1:1 ratio).
        //   Rescue path  - reuse last frame when output is starving (< 2 queued).
        // Re-read output status: capture DMA may have taken several ms.
        if (hasFrame)
        {
            device.AutoCirculateGetStatus(NTV2_CHANNEL2, os);
            if (os.acState == NTV2_AUTOCIRCULATE_RUNNING
                && os.acBufferLevel < 5
                && (newFrame || outStarving))
            {
                AUTOCIRCULATE_TRANSFER outXfer;
                outXfer.SetVideoBuffer(reinterpret_cast<ULWord*>(vBuf.data()), fSize);
                if (newFrame && cachedAudioBytes > 0)
                    outXfer.SetAudioBuffer(reinterpret_cast<ULWord*>(aBuf.data()),
                                          cachedAudioBytes);
                device.AutoCirculateTransfer(NTV2_CHANNEL2, outXfer) ? nOut++ : nDrop++;
            }
        }

        // Yield briefly when idle; the 4-frame pre-fill provides the safety margin.
        if (!newFrame)
            AJATime::Sleep(1);

        if (nIn > 0 && (nIn % 25) == 0)
            std::cout << "\rIn=" << nIn
                      << "  Out=" << nOut
                      << "  Drop=" << nDrop
                      << "  cap_buf=" << cs.acBufferLevel
                      << "  out_buf=" << os.acBufferLevel
                      << "    " << std::flush;
    }
"""

result = before + new_loop + after

with open(r"C:\Users\Java\Desktop\sdi3\main.cpp", "w", encoding="utf-8") as f:
    f.write(result)

print("OK — wrote", len(result), "chars")
