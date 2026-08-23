#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class MaliEglDmaBufViewer {
public:
    MaliEglDmaBufViewer(std::uint32_t frameWidth,
                        std::uint32_t frameHeight,
                        std::uint32_t displayWidth,
                        std::uint32_t displayHeight);
    ~MaliEglDmaBufViewer();

    MaliEglDmaBufViewer(const MaliEglDmaBufViewer &) = delete;
    MaliEglDmaBufViewer &operator=(const MaliEglDmaBufViewer &) = delete;

    bool setBuffers(const int *dmaBufFds, std::size_t bufferCount,
                    std::uint32_t stride);

    // start(), render(), and stop() must run on the display thread.
    bool start();
    bool render(std::size_t index);
    bool bufferComplete(std::size_t index);
    bool waitForBuffer(std::size_t index);
    void stop();

    const std::string &lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
