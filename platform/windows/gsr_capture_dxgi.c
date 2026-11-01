/* gsr_capture_dxgi.c — DXGI Desktop Duplication capture backend (Phase 6).
 *
 * Monitor-only fallback backend used when Windows Graphics Capture is
 * unavailable (older Windows, locked-down sessions, missing WinRT interop
 * runtime). Desktop Duplication is a plain DXGI/D3D11 COM interface, so
 * this backend is pure C — no C++/WinRT needed (unlike WGC).
 *
 * The gsr_capture vtable matches upstream's contract exactly (capture.h),
 * and the per-frame D3D11 texture is imported into the shared ANGLE GL
 * device via EGL_ANGLE_d3d_texture_client_buffer — identical to the WGC
 * backend's Phase 5b integration, so capture() draws into the unchanged
 * upstream color-conversion pipeline.
 *
 * Rotation: DD is the OPPOSITE of WGC. WGC delivers pre-rotated content;
 * IDXGIOutputDuplication::AcquireNextFrame returns an UN-rotated surface
 * (native panel orientation) with the desktop image rotated *within* it.
 * So the draw must pass the monitor's rotation and the rotated
 * (effective) size as source_size, exactly like upstream's KMS monitor
 * backend (capture_size = rotated, texture_size = native).
 *
 * Windows port addition — see docs/upstream-porting-notes.md §3g.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include "egl_win32.h"
#include "../../platform/include/capture.h"
#include "../../upstream/include/egl.h"
#include "../../upstream/include/log.h"
#include "../../upstream/include/color_conversion.h"
#include "../../upstream/include/capture/capture.h"
#include "../../upstream/include/utils.h" /* scale_keep_aspect_ratio, gsr_capture_get_target_position */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IID_IDXGIOutput1 — 00cddea8-939b-4b83-a340-a685226666cc. Defined locally
   rather than referencing mingw-w64's symbol (some DXGI IIDs are not
   linkable from libdxgi.a; see docs/upstream-porting-notes.md §3d). */
static const GUID GSR_IID_IDXGIOutput1 = {0x00cddea8, 0x939b, 0x4b83, {0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc}};
/* IID_ID3D11Texture2D — 6f15aaf2-d208-4e89-9ab4-489535d34f9c. */
static const GUID GSR_IID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

/* ---- pure logic (headless-tested) --------------------------------------- */

/* Maps a DXGI_MODE_ROTATION value to the gsr rotation enum. DXGI:
   UNSPECIFIED=0, IDENTITY=1, ROTATE90=2, ROTATE180=3, ROTATE270=4.
   GSR_ROT_90 is a 90° clockwise rotation (matches DXGI's), so the mapping
   is identity-minus-one. Returns GSR_PLATFORM_WGC_ROT_0 for garbage. */
gsr_platform_wgc_rotation gsr_platform_dxgi_rotation_from_dxgi(uint32_t dxgi_rotation) {
    switch(dxgi_rotation) {
        case 2: return GSR_PLATFORM_WGC_ROT_90;
        case 3: return GSR_PLATFORM_WGC_ROT_180;
        case 4: return GSR_PLATFORM_WGC_ROT_270;
        default: return GSR_PLATFORM_WGC_ROT_0;
    }
}

/* Whether the rotation swaps width and height (90°/270°). */
bool gsr_platform_dxgi_rotation_swaps_size(gsr_platform_wgc_rotation rotation) {
    return rotation == GSR_PLATFORM_WGC_ROT_90 || rotation == GSR_PLATFORM_WGC_ROT_270;
}

/* ---- backend state ------------------------------------------------------- */

typedef struct {
    gsr_platform_dxgi_target target;
    gsr_platform_dxgi_options options;
    gsr_egl *egl;

    ID3D11Device *device;
    IDXGIOutputDuplication *duplication;
    IDXGIOutput1 *output1;        /* owned when a standalone device was created */

    IDXGIResource *latest_resource; /* acquired frame (must ReleaseFrame) */
    ID3D11Texture2D *latest_texture;
    int frame_width, frame_height;  /* native (un-rotated) surface size */
    vec2i video_size;               /* effective (rotated) size */
    gsr_platform_wgc_rotation rotation;

    gsr_platform_wgc_damage damage;
    bool started;
    bool should_stop;
    bool stop_is_error;
    void *import_handle;

    /* ACCESS_LOST recovery. DXGI refuses an immediate re-DuplicateOutput
       (typically 0x80070005 while the desktop is mid-transition), so the
       re-create is retried with a delay instead of failing the recording. */
    ULONGLONG next_recreate_ms;
    int recreate_failures;
} gsr_capture_dxgi;

/* ---- vtable implementations ---------------------------------------------- */

static void dxgi_release_frame(gsr_capture_dxgi *self) {
    if(self->duplication && self->latest_resource) {
        self->duplication->lpVtbl->ReleaseFrame(self->duplication);
    }
    if(self->latest_resource) {
        self->latest_resource->lpVtbl->Release(self->latest_resource);
        self->latest_resource = NULL;
    }
    if(self->latest_texture) {
        self->latest_texture->lpVtbl->Release(self->latest_texture);
        self->latest_texture = NULL;
    }
}

/* Releases the duplication object only (keeps |output1| alive so the
   ACCESS_LOST path can re-create the duplication; dxgi_destroy_duplication
   releases |output1| too, at teardown). */
static void dxgi_release_duplication(gsr_capture_dxgi *self) {
    dxgi_release_frame(self);
    if(self->duplication) {
        self->duplication->lpVtbl->Release(self->duplication);
        self->duplication = NULL;
    }
}

static void dxgi_destroy_duplication(gsr_capture_dxgi *self) {
    dxgi_release_duplication(self);
    if(self->output1) {
        self->output1->lpVtbl->Release(self->output1);
        self->output1 = NULL;
    }
}

/* How long to keep retrying a lost duplication before failing the
   recording. DXGI only allows one duplication client per output, so the
   re-create can keep failing while another client (or a transitioning
   desktop: UAC, lock screen, display mode change, fullscreen exclusive
   switch) still owns it. A few seconds of retries covers all of those;
   giving up on the first attempt killed the recording permanently. */
#define GSR_DXGI_RECREATE_RETRY_MS 500
#define GSR_DXGI_RECREATE_MAX_FAILURES 20 /* ~10 s */

static bool dxgi_try_recreate_duplication(gsr_capture_dxgi *self) {
    if(!self->output1 || !self->device)
        return false;

    IDXGIOutputDuplication *duplication = NULL;
    const HRESULT hr = self->output1->lpVtbl->DuplicateOutput(self->output1, (IUnknown*)self->device, &duplication);
    if(FAILED(hr)) {
        ++self->recreate_failures;
        self->next_recreate_ms = GetTickCount64() + GSR_DXGI_RECREATE_RETRY_MS;
        if(self->recreate_failures == 1 || self->recreate_failures == GSR_DXGI_RECREATE_MAX_FAILURES)
            gsr_log(GSR_LOG_LEVEL_WARNING, "gsr_capture_dxgi_tick: DuplicateOutput failed (0x%08lx), retrying (attempt %d/%d)", (unsigned long)hr, self->recreate_failures, GSR_DXGI_RECREATE_MAX_FAILURES);

        if(self->recreate_failures >= GSR_DXGI_RECREATE_MAX_FAILURES) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_tick: could not recreate desktop duplication after %d attempts, stopping capture", self->recreate_failures);
            self->should_stop = true;
            self->stop_is_error = true;
        }
        return false;
    }

    self->duplication = duplication;
    self->recreate_failures = 0;
    self->next_recreate_ms = 0;
    gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_tick: desktop duplication recreated, capture resumed");
    return true;
}

/* Finds the IDXGIOutput whose HMONITOR matches the target. Returns an
   AddRef'd IDXGIOutput on success (caller releases). */
static IDXGIOutput *dxgi_find_output(HMONITOR hmonitor) {
    IDXGIFactory1 *factory = NULL;
    if(FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory)))
        return NULL;

    IDXGIOutput *found = NULL;
    for(UINT adapter_index = 0; ; ++adapter_index) {
        IDXGIAdapter *adapter = NULL;
        if(factory->lpVtbl->EnumAdapters(factory, adapter_index, &adapter) != S_OK)
            break;
        for(UINT output_index = 0; ; ++output_index) {
            IDXGIOutput *output = NULL;
            if(adapter->lpVtbl->EnumOutputs(adapter, output_index, &output) != S_OK)
                break;
            DXGI_OUTPUT_DESC desc;
            if(output->lpVtbl->GetDesc(output, &desc) == S_OK && desc.Monitor == hmonitor) {
                found = output;
                break;
            }
            output->lpVtbl->Release(output);
        }
        adapter->lpVtbl->Release(adapter);
        if(found)
            break;
    }
    factory->lpVtbl->Release(factory);
    return found;
}

/* Creates a D3D11 device (hardware, WARP fallback) with BGRA support (the
   ANGLE interop requirement — same policy as the egl/WGC backends). */
static ID3D11Device *dxgi_create_device(void) {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    /* VIDEO_SUPPORT is needed so the device can also drive NVENC (FFmpeg's
       d3d11va hwcontext requires it; see gsr_nvenc_win32.c). Fall back to
       BGRA-only on old drivers. */
    const UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_10_0;
    ID3D11Device *device = NULL;

    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
        levels, (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
        &device, &got_level, NULL);
    if(FAILED(hr)) {
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_start: video-support device unavailable (0x%08lx), retrying without it", (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
            &device, &got_level, NULL);
    }
    if(FAILED(hr)) {
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_start: hardware device unavailable (0x%08lx), using WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION,
            &device, &got_level, NULL);
    }
    if(FAILED(hr)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_start: D3D11CreateDevice failed (0x%08lx)", (unsigned long)hr);
        return NULL;
    }
    return device;
}

static int dxgi_start(gsr_capture *cap, gsr_capture_metadata *capture_metadata) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    if(self->started)
        return 0;

    /* 1. Device. Phase 5b: when a GL pipeline is configured, DD must run on
       the SAME device ANGLE runs on (zero-copy import). Standalone
       (self-test): hardware first, WARP fallback. */
    self->egl = self->options.egl;
    if(self->egl && self->egl->d3d11_device) {
        self->device = (ID3D11Device*)self->egl->d3d11_device;
        self->device->lpVtbl->AddRef(self->device);
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_start: using the shared ANGLE D3D11 device");
    } else {
        self->device = dxgi_create_device();
        if(!self->device)
            return -1;
    }

    /* 2. Find the output for the target monitor. */
    IDXGIOutput *output = dxgi_find_output((HMONITOR)self->target.hmonitor);
    if(!output) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_start: no DXGI output for monitor %s", self->target.name);
        self->should_stop = true;
        self->stop_is_error = true;
        return -1;
    }

    /* Monitor rotation (from the output desc — needed because DD surfaces
       are un-rotated, see the file header). */
    DXGI_OUTPUT_DESC output_desc;
    if(output->lpVtbl->GetDesc(output, &output_desc) == S_OK) {
        self->rotation = gsr_platform_dxgi_rotation_from_dxgi((uint32_t)output_desc.Rotation);
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_start: monitor rotation %d degrees",
            (int)(output_desc.Rotation == 0 ? 0 : (output_desc.Rotation - 1) * 90));
    } else {
        self->rotation = GSR_PLATFORM_WGC_ROT_0;
    }

    /* 3. DuplicateOutput. Requires the device on the SAME adapter as the
       output (a WARP device or a device on another adapter fails with
       DXGI_ERROR_UNSUPPORTED). */
    IDXGIOutput1 *output1 = NULL;
    if(FAILED(output->lpVtbl->QueryInterface(output, &GSR_IID_IDXGIOutput1, (void**)&output1))) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_start: IDXGIOutput1 unavailable (DXGI 1.2 needed)");
        output->lpVtbl->Release(output);
        self->should_stop = true;
        self->stop_is_error = true;
        return -1;
    }
    output->lpVtbl->Release(output);

    /* pDevice is IUnknown* — C has no implicit upcast, so cast explicitly. */
    HRESULT hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown*)self->device, &self->duplication);
    if(FAILED(hr)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_start: DuplicateOutput failed (0x%08lx) — desktop duplication unavailable for %s",
            (unsigned long)hr, self->target.name);
        output1->lpVtbl->Release(output1);
        self->should_stop = true;
        self->stop_is_error = true;
        return -1;
    }
    self->output1 = output1;

    /* 4. Metadata. DesktopCoordinates are already in the post-rotation
       (effective) coordinate space (Phase 4 lesson, §3d: "desktop coords =
       post-rotation"; the desktop-dup docs' portrait example reports the
       mode as 768x1024@90° while AcquireNextFrame returns the 1024x768
       native surface). So the effective video size is the desktop
       coordinates as-is — no swap — exactly like upstream KMS, where
       video_size is the rotated capture_size. */
    const int effective_w = (int)(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    const int effective_h = (int)(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
    self->video_size = (vec2i){effective_w, effective_h};
    capture_metadata->video_size = self->video_size;

    self->started = true;
    return 0;
}

static void dxgi_tick(gsr_capture *cap) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    if(!self->started || self->should_stop)
        return;

    /* Device loss: hard stop. */
    if(self->device) {
        const HRESULT removed = self->device->lpVtbl->GetDeviceRemovedReason(self->device);
        if(FAILED(removed)) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_tick: D3D11 device removed (0x%08lx)", (unsigned long)removed);
            self->should_stop = true;
            self->stop_is_error = true;
            return;
        }
    }

    /* No live duplication: either ACCESS_LOST tore it down or a re-create
       has not succeeded yet. Retry on a delay and produce no frame
       meanwhile (the recorder keeps its cadence). */
    if(!self->duplication) {
        if(GetTickCount64() >= self->next_recreate_ms)
            dxgi_try_recreate_duplication(self);
        return;
    }

    /* Release the previous frame before the next acquire (the DD contract:
       ReleaseFrame must be called before the next AcquireNextFrame). */
    dxgi_release_frame(self);

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    memset(&frame_info, 0, sizeof(frame_info));
    IDXGIResource *resource = NULL;
    const HRESULT hr = self->duplication->lpVtbl->AcquireNextFrame(self->duplication, 0, &frame_info, &resource);

    if(hr == DXGI_ERROR_WAIT_TIMEOUT)
        return; /* desktop unchanged — no new frame */

    if(hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        /* The desktop switched out from under us (resolution/session
           change, secure desktop, a fullscreen exclusive app, or another
           duplication client taking the output). Drop the duplication and
           let the retry path above re-create it — this is a soft re-acquire,
           not a reason to end the recording. DXGI refuses an immediate
           re-create while the desktop is transitioning, so wait one retry
           interval before the first attempt. */
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_capture_dxgi_tick: desktop access lost (0x%08lx), recreating duplication", (unsigned long)hr);
        dxgi_release_duplication(self); /* keeps |output1| for re-create */
        self->next_recreate_ms = GetTickCount64() + GSR_DXGI_RECREATE_RETRY_MS;
        return;
    }

    if(FAILED(hr)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_tick: AcquireNextFrame failed (0x%08lx)", (unsigned long)hr);
        self->should_stop = true;
        self->stop_is_error = true;
        return;
    }

    /* QI the frame's IDXGIResource to the D3D11 texture. */
    ID3D11Texture2D *texture = NULL;
    if(FAILED(resource->lpVtbl->QueryInterface(resource, &GSR_IID_ID3D11Texture2D, (void**)&texture))) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_tick: frame is not a D3D11 texture");
        resource->lpVtbl->Release(resource);
        return;
    }
    self->latest_resource = resource;
    self->latest_texture = texture;

    D3D11_TEXTURE2D_DESC desc;
    texture->lpVtbl->GetDesc(texture, &desc);
    self->frame_width = (int)desc.Width;
    self->frame_height = (int)desc.Height;

    gsr_platform_wgc_damage_on_frame(&self->damage);
}

static bool dxgi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    if(err)
        *err = self->stop_is_error;
    return self->should_stop;
}

static int dxgi_capture(gsr_capture *cap, gsr_capture_metadata *capture_metadata, gsr_color_conversion *color_conversion) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;

    /* The recorder calls clear_damage() BEFORE capture() (recorder.c:
       recorder_capture_and_encode_frame), so capture() must not gate on the
       damage flag — it delivers the latest frame whenever one exists.
       Returns -1 only when no frame has arrived yet. */
    if(!self->latest_texture)
        return -1;

    /* Phase 5b integration: import the DD texture into GL (zero-copy on the
       shared device) and draw it into the color conversion. When no GL
       pipeline is configured (standalone self-test), a frame was still
       acquired — expose it via the self-test's direct texture read. */
    gsr_egl *egl = color_conversion ? color_conversion->params.egl : NULL;
    if(!egl)
        return 0;

    if(!self->import_handle) {
        self->import_handle = gsr_platform_egl_import_texture(egl, self->latest_texture);
        if(!self->import_handle) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_capture: failed to import DD texture into GL");
            return -1;
        }
    } else if(!gsr_platform_egl_update_texture(egl, self->import_handle, self->latest_texture)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_capture_dxgi_capture: failed to update imported GL texture");
        return -1;
    }

    const unsigned int texture_id = gsr_platform_egl_texture_id(egl, self->import_handle);
    const vec2i frame_size = {(int)self->frame_width, (int)self->frame_height};
    vec2i recording_size = capture_metadata->recording_size;
    if(recording_size.x <= 0 || recording_size.y <= 0)
        recording_size = capture_metadata->video_size;
    const vec2i output_size = scale_keep_aspect_ratio(self->video_size, recording_size);
    const vec2i target_pos = gsr_capture_get_target_position(output_size, capture_metadata);

    /* DD delivers an UN-rotated surface with the desktop image rotated
       within it, so the draw rotation is the monitor's rotation and
       source_size is the rotated (effective) size — exactly the upstream
       KMS monitor pattern. The D3D11 texture imports as GL_TEXTURE_2D, so
       external_texture=false. Surface format is always B8G8R8A8 =
       GSR_SOURCE_COLOR_BGR. */
    gsr_color_conversion_draw(color_conversion, texture_id,
        target_pos, output_size,
        (vec2i){0, 0}, self->video_size, frame_size,
        (gsr_rotation)self->rotation, capture_metadata->flip, GSR_SOURCE_COLOR_BGR, false);
    return 0;
}

static bool dxgi_uses_external_image(gsr_capture *cap) {
    (void)cap;
    /* The D3D11 texture imports as a regular GL_TEXTURE_2D (Phase 5b), so
       the color conversion must NOT load the external-image (OES) shader
       and capture() draws with external_texture=false. */
    return false;
}

static bool dxgi_set_hdr_metadata(gsr_capture *cap, AVMasteringDisplayMetadata *mastering_display_metadata, AVContentLightMetadata *light_metadata) {
    (void)mastering_display_metadata;
    (void)light_metadata;
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    /* DD surfaces are always B8G8R8A8_UNORM (SDR) regardless of display
       mode — HDR cannot be captured through this backend, so report false
       even when the target is HDR. */
    (void)self;
    return false;
}

static bool dxgi_is_damaged(gsr_capture *cap) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    return gsr_platform_wgc_damage_is_damaged(&self->damage);
}

static void dxgi_clear_damage(gsr_capture *cap) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    gsr_platform_wgc_damage_consume(&self->damage);
}

static void dxgi_destroy(gsr_capture *cap) {
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    if(self) {
        if(self->import_handle && self->egl) {
            gsr_platform_egl_destroy_imported_texture(self->egl, self->import_handle);
            self->import_handle = NULL;
        }
        dxgi_destroy_duplication(self);
        if(self->device) {
            self->device->lpVtbl->Release(self->device);
            self->device = NULL;
        }
        free(self);
        cap->priv = NULL;
    }
    free(cap);
}

/* ---- extern "C" API (platform/include/capture.h) ------------------------- */

gsr_capture *gsr_platform_capture_dxgi_create(const gsr_platform_dxgi_target *target, const gsr_platform_dxgi_options *options) {
    if(!target || !target->hmonitor) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_platform_capture_dxgi_create: target monitor is NULL");
        return NULL;
    }

    gsr_capture *cap = (gsr_capture*)calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_dxgi *self = (gsr_capture_dxgi*)calloc(1, sizeof(gsr_capture_dxgi));
    if(!self) {
        free(cap);
        return NULL;
    }
    self->target = *target;
    if(options)
        self->options = *options;
    gsr_platform_wgc_damage_init(&self->damage);

    *cap = (gsr_capture){
        .start = dxgi_start,
        .tick = dxgi_tick,
        .should_stop = dxgi_should_stop,
        .capture = dxgi_capture,
        .uses_external_image = dxgi_uses_external_image,
        .set_hdr_metadata = dxgi_set_hdr_metadata,
        .is_damaged = dxgi_is_damaged,
        .clear_damage = dxgi_clear_damage,
        .destroy = dxgi_destroy,
        .priv = self,
    };
    return cap;
}

/* Latest captured frame accessor (same contract as the WGC one): the D3D11
   texture of the most recent DD frame and its native (un-rotated) size.
   Returns false when no frame yet. */
bool gsr_platform_capture_dxgi_get_frame(gsr_capture *cap, void **out_texture, int *width, int *height) {
    if(!cap || !cap->priv)
        return false;
    gsr_capture_dxgi *self = (gsr_capture_dxgi*)cap->priv;
    if(!self->latest_texture)
        return false;
    if(out_texture)
        *out_texture = self->latest_texture;
    if(width)
        *width = self->frame_width;
    if(height)
        *height = self->frame_height;
    return true;
}

/* Whether Desktop Duplication actually works on this system: a hardware
   D3D11 device can DuplicateOutput the primary monitor's output. */
bool gsr_platform_capture_dxgi_available(void) {
    /* Find the primary monitor's output. */
    HMONITOR primary = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    if(!primary)
        return false;
    IDXGIOutput *output = dxgi_find_output(primary);
    if(!output)
        return false;

    IDXGIOutput1 *output1 = NULL;
    if(FAILED(output->lpVtbl->QueryInterface(output, &GSR_IID_IDXGIOutput1, (void**)&output1))) {
        output->lpVtbl->Release(output);
        return false;
    }
    output->lpVtbl->Release(output);

    ID3D11Device *device = dxgi_create_device();
    if(!device) {
        output1->lpVtbl->Release(output1);
        return false;
    }

    IDXGIOutputDuplication *duplication = NULL;
    const HRESULT hr = output1->lpVtbl->DuplicateOutput(output1, (IUnknown*)device, &duplication);
    if(SUCCEEDED(hr) && duplication)
        duplication->lpVtbl->Release(duplication);
    else if(FAILED(hr))
        gsr_log(GSR_LOG_LEVEL_INFO, "gsr_platform_capture_dxgi_available: DuplicateOutput failed (0x%08lx) — Desktop Duplication unavailable",
            (unsigned long)hr);

    device->lpVtbl->Release(device);
    output1->lpVtbl->Release(output1);
    return SUCCEEDED(hr);
}
