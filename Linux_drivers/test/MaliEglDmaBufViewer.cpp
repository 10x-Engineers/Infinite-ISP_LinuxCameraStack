#include "MaliEglDmaBufViewer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <drm/drm_fourcc.h>

#include <cstdlib>
#include <fstream>
#include <array>
#include <sstream>
#include <vector>

namespace {

// Mali r9p0 accepts the RGB565 views with a shared BGR24 pitch, but plane
// offsets that are not aligned to 128 bytes are interpreted at the wrong byte
// phase.  A BGR pixel pair occupies six bytes, so aligning a chunk boundary to
// 64 pairs satisfies both the 128-byte import alignment and pair integrity.
constexpr std::uint32_t kMaliPlaneOffsetAlignment = 128;
constexpr std::uint32_t kBgrPairBytes = 6;
constexpr std::uint32_t kChunkPairAlignment = 64;
static_assert((kChunkPairAlignment * kBgrPairBytes) %
                  kMaliPlaneOffsetAlignment == 0,
              "Mali chunk boundaries must produce aligned byte offsets");

std::uint32_t nearestAlignedPairBoundary(std::uint32_t pair) {
    return ((pair + kChunkPairAlignment / 2) / kChunkPairAlignment) *
           kChunkPairAlignment;
}

std::string eglError(const char *operation) {
    std::ostringstream stream;
    stream << operation << " failed (EGL error 0x" << std::hex
           << eglGetError() << ')';
    return stream.str();
}

GLuint compileShader(GLenum type, const char *source, std::string &error) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<char> log(static_cast<std::size_t>(length) + 1);
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    error = std::string("shader compilation failed: ") + log.data();
    glDeleteShader(shader);
    return 0;
}

} // namespace

struct MaliEglDmaBufViewer::Impl {
    struct Buffer {
        int fd = -1;
        std::uint32_t stride = 0;
        std::array<EGLImageKHR, 3> images{
            EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
        std::array<GLuint, 3> textures{};
        EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    };

    std::uint32_t frameWidth;
    std::uint32_t frameHeight;
    std::uint32_t displayWidth;
    std::uint32_t displayHeight;
    std::array<std::uint32_t, 3> chunkPairs{};
    std::array<std::uint32_t, 3> chunkWords{};
    std::array<std::uint32_t, 3> chunkByteOffsets{};
    std::string error;

    std::vector<Buffer> buffers;

    Display *xDisplay = nullptr;
    Window window = 0;
    Colormap colormap = 0;
    Atom wmDelete = 0;
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext eglContext = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC clientWaitSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    GLuint program = 0;
    GLint positionAttribute = -1;
    GLint textureAttribute = -1;
    bool started = false;
    std::string capturePath;
    bool captureDone = false;

    Impl(std::uint32_t inputWidth, std::uint32_t inputHeight,
         std::uint32_t outputWidth, std::uint32_t outputHeight)
        : frameWidth(inputWidth), frameHeight(inputHeight),
          displayWidth(outputWidth), displayHeight(outputHeight) {}

    bool createProgram() {
        static constexpr char kVertexShader[] = R"(
            attribute vec2 position;
            attribute vec2 texcoord;
            varying vec2 video_texcoord;
            void main() {
                gl_Position = vec4(position, 0.0, 1.0);
                video_texcoord = texcoord;
            }
        )";
        static constexpr char kFragmentShader[] = R"(
            precision mediump float;
            uniform sampler2D video0;
            uniform sampler2D video1;
            uniform sampler2D video2;
            uniform float frame_width;
            uniform float frame_height;
            uniform float chunk_pairs0;
            uniform float chunk_pairs1;
            uniform float chunk_words0;
            uniform float chunk_words1;
            uniform float chunk_words2;
            varying vec2 video_texcoord;

            vec2 read_word(float word_index, float row, float chunk) {
                vec3 encoded;
                if (chunk < 0.5) {
                    vec2 uv = vec2((word_index + 0.5) / chunk_words0,
                                   (row + 0.5) / frame_height);
                    encoded = texture2D(video0, uv).rgb;
                } else if (chunk < 1.5) {
                    vec2 uv = vec2((word_index + 0.5) / chunk_words1,
                                   (row + 0.5) / frame_height);
                    encoded = texture2D(video1, uv).rgb;
                } else {
                    vec2 uv = vec2((word_index + 0.5) / chunk_words2,
                                   (row + 0.5) / frame_height);
                    encoded = texture2D(video2, uv).rgb;
                }
                vec3 bits = floor(encoded * vec3(31.0, 63.0, 31.0) + 0.5);
                // Return the original little-endian bytes without ever
                // constructing a 16-bit integer (Mali-400 fragment mediump).
                float low_byte = mod(bits.g, 8.0) * 32.0 + bits.b;
                float high_byte = bits.r * 8.0 + floor(bits.g / 8.0);
                return vec2(low_byte, high_byte);
            }

            void main() {
                float pixel = min(floor(video_texcoord.x * frame_width),
                                  frame_width - 1.0);
                float row = min(floor(video_texcoord.y * frame_height),
                                frame_height - 1.0);
                float pair = floor(pixel * 0.5);
                float local_pair;
                float chunk;
                if (pair < chunk_pairs0) {
                    chunk = 0.0;
                    local_pair = pair;
                } else if (pair < chunk_pairs0 + chunk_pairs1) {
                    chunk = 1.0;
                    local_pair = pair - chunk_pairs0;
                } else {
                    chunk = 2.0;
                    local_pair = pair - chunk_pairs0 - chunk_pairs1;
                }

                float word_base = local_pair * 3.0;
                bool odd = mod(pixel, 2.0) >= 0.5;
                float first_index = word_base + (odd ? 1.0 : 0.0);
                vec2 first = read_word(first_index, row, chunk);
                vec2 second = read_word(first_index + 1.0, row, chunk);

                vec3 rgb;
                if (!odd) {
                    // Even BGR24 pixel: [B,G] in word N, [R,...] in N+1.
                    rgb = vec3(second.x, first.y, first.x);
                } else {
                    // Odd BGR24 pixel: [...,B] in word N, [G,R] in N+1.
                    rgb = vec3(second.y, second.x, first.y);
                }
                gl_FragColor = vec4(rgb / 255.0, 1.0);
            }
        )";

        const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader,
                                            error);
        if (!vertex)
            return false;
        const GLuint fragment = compileShader(GL_FRAGMENT_SHADER,
                                              kFragmentShader, error);
        if (!fragment) {
            glDeleteShader(vertex);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(static_cast<std::size_t>(length) + 1);
            glGetProgramInfoLog(program, length, nullptr, log.data());
            error = std::string("shader link failed: ") + log.data();
            return false;
        }

        positionAttribute = glGetAttribLocation(program, "position");
        textureAttribute = glGetAttribLocation(program, "texcoord");
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "video0"), 0);
        glUniform1i(glGetUniformLocation(program, "video1"), 1);
        glUniform1i(glGetUniformLocation(program, "video2"), 2);
        glUniform1f(glGetUniformLocation(program, "frame_width"), frameWidth);
        glUniform1f(glGetUniformLocation(program, "frame_height"), frameHeight);
        glUniform1f(glGetUniformLocation(program, "chunk_pairs0"),
                    chunkPairs[0]);
        glUniform1f(glGetUniformLocation(program, "chunk_pairs1"),
                    chunkPairs[1]);
        glUniform1f(glGetUniformLocation(program, "chunk_words0"),
                    chunkWords[0]);
        glUniform1f(glGetUniformLocation(program, "chunk_words1"),
                    chunkWords[1]);
        glUniform1f(glGetUniformLocation(program, "chunk_words2"),
                    chunkWords[2]);
        return positionAttribute >= 0 && textureAttribute >= 0;
    }
};

MaliEglDmaBufViewer::MaliEglDmaBufViewer(std::uint32_t frameWidth,
                                         std::uint32_t frameHeight,
                                         std::uint32_t displayWidth,
                                         std::uint32_t displayHeight)
    : impl_(std::make_unique<Impl>(frameWidth, frameHeight, displayWidth,
                                  displayHeight)) {}

MaliEglDmaBufViewer::~MaliEglDmaBufViewer() {
    stop();
}

bool MaliEglDmaBufViewer::setBuffers(const int *dmaBufFds,
                                     std::size_t bufferCount,
                                     std::uint32_t stride) {
    impl_->error.clear();
    if (!dmaBufFds || bufferCount == 0 || stride == 0) {
        impl_->error = "invalid VIP DMA-BUF list";
        return false;
    }
    if ((impl_->frameWidth & 1U) != 0 ||
        stride < impl_->frameWidth * 3) {
        impl_->error = "BGR24 zero-copy view requires an even width and a "
                       "stride covering all active pixels";
        return false;
    }

    const std::uint32_t totalPairs = impl_->frameWidth / 2;
    const std::uint32_t firstBoundary =
        nearestAlignedPairBoundary(totalPairs / 3);
    const std::uint32_t secondBoundary =
        nearestAlignedPairBoundary(totalPairs * 2 / 3);
    if (firstBoundary == 0 || secondBoundary <= firstBoundary ||
        secondBoundary >= totalPairs) {
        impl_->error = "BGR24 frame is too narrow for three aligned Mali "
                       "DMA-BUF views";
        return false;
    }
    impl_->chunkPairs = {
        firstBoundary,
        secondBoundary - firstBoundary,
        totalPairs - secondBoundary,
    };

    std::uint32_t byteOffset = 0;
    for (std::size_t chunk = 0; chunk < 3; ++chunk) {
        if ((byteOffset % kMaliPlaneOffsetAlignment) != 0) {
            impl_->error = "internal Mali DMA-BUF chunk alignment error";
            return false;
        }
        impl_->chunkWords[chunk] = impl_->chunkPairs[chunk] * 3;
        impl_->chunkByteOffsets[chunk] = byteOffset;
        byteOffset += impl_->chunkPairs[chunk] * kBgrPairBytes;
    }
    impl_->buffers.clear();
    impl_->buffers.reserve(bufferCount);
    for (std::size_t i = 0; i < bufferCount; ++i) {
        Impl::Buffer buffer;
        if (dmaBufFds[i] < 0) {
            impl_->error = "VIP returned an invalid DMA-BUF fd";
            impl_->buffers.clear();
            return false;
        }
        buffer.fd = dmaBufFds[i];
        buffer.stride = stride;
        impl_->buffers.push_back(buffer);
    }
    return true;
}

bool MaliEglDmaBufViewer::start() {
    impl_->error.clear();
    if (impl_->started)
        return true;
    if (impl_->buffers.empty()) {
        impl_->error = "display buffers were not allocated";
        return false;
    }

    XInitThreads();
    impl_->xDisplay = XOpenDisplay(nullptr);
    if (!impl_->xDisplay) {
        impl_->error = "XOpenDisplay failed; check DISPLAY";
        return false;
    }
    impl_->eglDisplay = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(impl_->xDisplay));
    EGLint major = 0;
    EGLint minor = 0;
    if (impl_->eglDisplay == EGL_NO_DISPLAY ||
        !eglInitialize(impl_->eglDisplay, &major, &minor)) {
        impl_->error = eglError("eglInitialize");
        stop();
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        impl_->error = eglError("eglBindAPI");
        stop();
        return false;
    }

    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 5,
        EGL_GREEN_SIZE, 6,
        EGL_BLUE_SIZE, 5,
        EGL_NONE
    };
    EGLConfig configs[64]{};
    EGLint configCount = 0;
    if (!eglChooseConfig(impl_->eglDisplay, configAttributes, configs, 64,
                         &configCount)) {
        impl_->error = eglError("eglChooseConfig");
        stop();
        return false;
    }

    const int screen = DefaultScreen(impl_->xDisplay);
    const VisualID rootVisual = XVisualIDFromVisual(
        DefaultVisual(impl_->xDisplay, screen));
    EGLConfig config = nullptr;
    for (EGLint i = 0; i < configCount; ++i) {
        EGLint visual = 0;
        eglGetConfigAttrib(impl_->eglDisplay, configs[i], EGL_NATIVE_VISUAL_ID,
                           &visual);
        if (static_cast<VisualID>(visual) == rootVisual) {
            config = configs[i];
            break;
        }
    }
    if (!config) {
        impl_->error = "no Mali EGL config matches the X11 root visual";
        stop();
        return false;
    }

    XSetWindowAttributes attributes{};
    impl_->colormap = XCreateColormap(
        impl_->xDisplay, RootWindow(impl_->xDisplay, screen),
        DefaultVisual(impl_->xDisplay, screen), AllocNone);
    attributes.colormap = impl_->colormap;
    attributes.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
    impl_->window = XCreateWindow(
        impl_->xDisplay, RootWindow(impl_->xDisplay, screen), 0, 0,
        impl_->displayWidth, impl_->displayHeight, 0,
        DefaultDepth(impl_->xDisplay, screen), InputOutput,
        DefaultVisual(impl_->xDisplay, screen), CWColormap | CWEventMask,
        &attributes);
    XStoreName(impl_->xDisplay, impl_->window,
               "Infinite ISP - Mali zero-copy");
    impl_->wmDelete = XInternAtom(impl_->xDisplay, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(impl_->xDisplay, impl_->window, &impl_->wmDelete, 1);
    XMapWindow(impl_->xDisplay, impl_->window);
    XFlush(impl_->xDisplay);

    impl_->eglSurface = eglCreateWindowSurface(
        impl_->eglDisplay, config,
        static_cast<EGLNativeWindowType>(impl_->window), nullptr);
    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    impl_->eglContext = eglCreateContext(
        impl_->eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
    if (impl_->eglSurface == EGL_NO_SURFACE ||
        impl_->eglContext == EGL_NO_CONTEXT ||
        !eglMakeCurrent(impl_->eglDisplay, impl_->eglSurface,
                        impl_->eglSurface, impl_->eglContext)) {
        impl_->error = eglError("creating the Mali EGL context");
        stop();
        return false;
    }
    // Synchronize buffer swaps with X11's display refresh by default.  Without
    // this, a moving subject exposes a horizontal tear where two camera frames
    // are scanned out at once.  Keep an override for driver benchmarking.
    int swapInterval = 1;
    if (const char *value = std::getenv("ISP_MALI_VSYNC"))
        swapInterval = std::atoi(value) != 0 ? 1 : 0;
    if (!eglSwapInterval(impl_->eglDisplay, swapInterval)) {
        impl_->error = eglError("eglSwapInterval");
        stop();
        return false;
    }

    impl_->createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    impl_->destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    impl_->imageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    impl_->createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    impl_->clientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(
        eglGetProcAddress("eglClientWaitSyncKHR"));
    impl_->destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    if (!impl_->createImage || !impl_->destroyImage ||
        !impl_->imageTargetTexture || !impl_->createSync ||
        !impl_->clientWaitSync || !impl_->destroySync) {
        impl_->error = "Mali EGLImage/fence entry points are unavailable";
        stop();
        return false;
    }
    if (!impl_->createProgram()) {
        stop();
        return false;
    }

    for (auto &buffer : impl_->buffers) {
        // Mali r9p0 rejects RGB888 imports. Split each BGR24 line into three
        // pair- and plane-offset-aligned RGB565 EGLImage views. The chunks may
        // have different widths (for example 256+320+244 pairs at 1640
        // pixels), which keeps every two-pixel/six-byte group intact while
        // satisfying Mali's 128-byte offset requirement.
        for (int chunk = 0; chunk < 3; ++chunk) {
            const EGLint imageAttributes[] = {
                EGL_WIDTH, static_cast<EGLint>(impl_->chunkWords[chunk]),
                EGL_HEIGHT, static_cast<EGLint>(impl_->frameHeight),
                EGL_LINUX_DRM_FOURCC_EXT,
                    static_cast<EGLint>(DRM_FORMAT_RGB565),
                EGL_DMA_BUF_PLANE0_FD_EXT, buffer.fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                    static_cast<EGLint>(impl_->chunkByteOffsets[chunk]),
                EGL_DMA_BUF_PLANE0_PITCH_EXT,
                    static_cast<EGLint>(buffer.stride),
                EGL_NONE
            };
            buffer.images[chunk] = impl_->createImage(
                impl_->eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                nullptr, imageAttributes);
            if (buffer.images[chunk] == EGL_NO_IMAGE_KHR) {
                impl_->error = eglError("eglCreateImageKHR(DMA-BUF)");
                stop();
                return false;
            }
            glGenTextures(1, &buffer.textures[chunk]);
            glBindTexture(GL_TEXTURE_2D, buffer.textures[chunk]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
            impl_->imageTargetTexture(GL_TEXTURE_2D,
                                      buffer.images[chunk]);
            if (glGetError() != GL_NO_ERROR) {
                impl_->error = "glEGLImageTargetTexture2DOES failed";
                stop();
                return false;
            }
        }
    }

    EGLint surfaceWidth = static_cast<EGLint>(impl_->displayWidth);
    EGLint surfaceHeight = static_cast<EGLint>(impl_->displayHeight);
    if (eglQuerySurface(impl_->eglDisplay, impl_->eglSurface, EGL_WIDTH,
                        &surfaceWidth) &&
        eglQuerySurface(impl_->eglDisplay, impl_->eglSurface, EGL_HEIGHT,
                        &surfaceHeight) &&
        surfaceWidth > 0 && surfaceHeight > 0) {
        impl_->displayWidth = static_cast<std::uint32_t>(surfaceWidth);
        impl_->displayHeight = static_cast<std::uint32_t>(surfaceHeight);
    }
    glViewport(0, 0, impl_->displayWidth, impl_->displayHeight);
    impl_->started = true;
    return true;
}

bool MaliEglDmaBufViewer::render(std::size_t index) {
    if (!impl_->started || index >= impl_->buffers.size()) {
        impl_->error = "invalid Mali display buffer index";
        return false;
    }
    while (XPending(impl_->xDisplay)) {
        XEvent event{};
        XNextEvent(impl_->xDisplay, &event);
        if (event.type == ClientMessage &&
            static_cast<Atom>(event.xclient.data.l[0]) == impl_->wmDelete) {
            impl_->error.clear();
            return false;
        }
        if (event.type == ConfigureNotify && event.xconfigure.width > 0 &&
            event.xconfigure.height > 0 &&
            (static_cast<std::uint32_t>(event.xconfigure.width) !=
                 impl_->displayWidth ||
             static_cast<std::uint32_t>(event.xconfigure.height) !=
                 impl_->displayHeight)) {
            impl_->displayWidth =
                static_cast<std::uint32_t>(event.xconfigure.width);
            impl_->displayHeight =
                static_cast<std::uint32_t>(event.xconfigure.height);
            glViewport(0, 0, impl_->displayWidth, impl_->displayHeight);
        }
    }

    // V4L2 rows start at the top; flip the texture vertically for OpenGL.
    static constexpr GLfloat vertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    glUseProgram(impl_->program);
    for (int chunk = 0; chunk < 3; ++chunk) {
        glActiveTexture(GL_TEXTURE0 + chunk);
        glBindTexture(GL_TEXTURE_2D,
                      impl_->buffers[index].textures[chunk]);
    }
    glEnableVertexAttribArray(impl_->positionAttribute);
    glEnableVertexAttribArray(impl_->textureAttribute);
    glVertexAttribPointer(impl_->positionAttribute, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(impl_->textureAttribute, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), vertices + 2);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (!impl_->captureDone) {
        if (impl_->capturePath.empty()) {
            if (const char *path = std::getenv("ISP_MALI_CAPTURE_PPM"))
                impl_->capturePath = path;
        }
        if (!impl_->capturePath.empty()) {
            std::vector<unsigned char> rgba(
                static_cast<std::size_t>(impl_->displayWidth) *
                impl_->displayHeight * 4);
            glReadPixels(0, 0, impl_->displayWidth, impl_->displayHeight,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            if (glGetError() != GL_NO_ERROR) {
                impl_->error = "glReadPixels for ISP_MALI_CAPTURE_PPM failed";
                return false;
            }
            std::ofstream output(impl_->capturePath, std::ios::binary);
            if (!output) {
                impl_->error = "cannot open ISP_MALI_CAPTURE_PPM output";
                return false;
            }
            output << "P6\n" << impl_->displayWidth << ' '
                   << impl_->displayHeight << "\n255\n";
            for (int y = static_cast<int>(impl_->displayHeight) - 1;
                 y >= 0; --y) {
                for (std::uint32_t x = 0; x < impl_->displayWidth; ++x) {
                    const auto offset =
                        (static_cast<std::size_t>(y) * impl_->displayWidth + x)
                        * 4;
                    output.write(reinterpret_cast<const char *>(rgba.data() +
                                                                 offset),
                                 3);
                }
            }
            impl_->captureDone = true;
        }
    }
    auto &buffer = impl_->buffers[index];
    if (buffer.sync != EGL_NO_SYNC_KHR) {
        impl_->error = "display buffer still has an active Mali fence";
        return false;
    }
    buffer.sync = impl_->createSync(impl_->eglDisplay, EGL_SYNC_FENCE_KHR,
                                    nullptr);
    if (buffer.sync == EGL_NO_SYNC_KHR) {
        impl_->error = eglError("eglCreateSyncKHR");
        return false;
    }
    glFlush();
    if (!eglSwapBuffers(impl_->eglDisplay, impl_->eglSurface)) {
        impl_->error = eglError("eglSwapBuffers");
        return false;
    }

    return true;
}

bool MaliEglDmaBufViewer::bufferComplete(std::size_t index) {
    if (!impl_->started || index >= impl_->buffers.size()) {
        impl_->error = "invalid Mali display buffer index";
        return false;
    }
    auto &buffer = impl_->buffers[index];
    if (buffer.sync == EGL_NO_SYNC_KHR)
        return true;
    const EGLint result = impl_->clientWaitSync(
        impl_->eglDisplay, buffer.sync, 0, 0);
    if (result == EGL_TIMEOUT_EXPIRED_KHR)
        return false;
    if (result != EGL_CONDITION_SATISFIED_KHR) {
        impl_->error = eglError("eglClientWaitSyncKHR");
        return false;
    }
    impl_->destroySync(impl_->eglDisplay, buffer.sync);
    buffer.sync = EGL_NO_SYNC_KHR;
    return true;
}

bool MaliEglDmaBufViewer::waitForBuffer(std::size_t index) {
    if (!impl_->started || index >= impl_->buffers.size()) {
        impl_->error = "invalid Mali display buffer index";
        return false;
    }
    auto &buffer = impl_->buffers[index];
    if (buffer.sync == EGL_NO_SYNC_KHR)
        return true;
    const EGLint result = impl_->clientWaitSync(
        impl_->eglDisplay, buffer.sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
        EGL_FOREVER_KHR);
    if (result != EGL_CONDITION_SATISFIED_KHR) {
        impl_->error = eglError("waiting for Mali display fence");
        return false;
    }
    impl_->destroySync(impl_->eglDisplay, buffer.sync);
    buffer.sync = EGL_NO_SYNC_KHR;
    return true;
}

void MaliEglDmaBufViewer::stop() {
    if (!impl_)
        return;
    if (impl_->eglDisplay != EGL_NO_DISPLAY &&
        impl_->eglContext != EGL_NO_CONTEXT) {
        eglMakeCurrent(impl_->eglDisplay, impl_->eglSurface,
                       impl_->eglSurface, impl_->eglContext);
        glFinish();
        for (auto &buffer : impl_->buffers) {
            if (buffer.sync != EGL_NO_SYNC_KHR && impl_->destroySync)
                impl_->destroySync(impl_->eglDisplay, buffer.sync);
            buffer.sync = EGL_NO_SYNC_KHR;
            for (int chunk = 0; chunk < 3; ++chunk) {
                if (buffer.textures[chunk])
                    glDeleteTextures(1, &buffer.textures[chunk]);
                buffer.textures[chunk] = 0;
                if (buffer.images[chunk] != EGL_NO_IMAGE_KHR &&
                    impl_->destroyImage) {
                    impl_->destroyImage(impl_->eglDisplay,
                                        buffer.images[chunk]);
                }
                buffer.images[chunk] = EGL_NO_IMAGE_KHR;
            }
        }
        if (impl_->program)
            glDeleteProgram(impl_->program);
        impl_->program = 0;
        eglMakeCurrent(impl_->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    }
    if (impl_->eglDisplay != EGL_NO_DISPLAY &&
        impl_->eglContext != EGL_NO_CONTEXT)
        eglDestroyContext(impl_->eglDisplay, impl_->eglContext);
    if (impl_->eglDisplay != EGL_NO_DISPLAY &&
        impl_->eglSurface != EGL_NO_SURFACE)
        eglDestroySurface(impl_->eglDisplay, impl_->eglSurface);
    if (impl_->eglDisplay != EGL_NO_DISPLAY)
        eglTerminate(impl_->eglDisplay);
    impl_->eglContext = EGL_NO_CONTEXT;
    impl_->eglSurface = EGL_NO_SURFACE;
    impl_->eglDisplay = EGL_NO_DISPLAY;

    if (impl_->xDisplay && impl_->window)
        XDestroyWindow(impl_->xDisplay, impl_->window);
    if (impl_->xDisplay && impl_->colormap)
        XFreeColormap(impl_->xDisplay, impl_->colormap);
    if (impl_->xDisplay)
        XCloseDisplay(impl_->xDisplay);
    impl_->window = 0;
    impl_->colormap = 0;
    impl_->xDisplay = nullptr;
    impl_->started = false;
}

const std::string &MaliEglDmaBufViewer::lastError() const {
    return impl_->error;
}
