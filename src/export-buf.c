#include "export-buf.h"
#include <stdio.h>
#include <ffnvcodec/dynlink_loader.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>

#if defined __has_include && __has_include(<libdrm/drm.h>)
#  include <libdrm/drm.h>
#  include <libdrm/drm_fourcc.h>
#else
#  include <drm/drm.h>
#  include <drm/drm_fourcc.h>
#endif

#ifndef EGL_NV_stream_consumer_eglimage
#define EGL_NV_stream_consumer_eglimage 1
#define EGL_STREAM_CONSUMER_IMAGE_NV      0x3373
#define EGL_STREAM_IMAGE_ADD_NV           0x3374
#define EGL_STREAM_IMAGE_REMOVE_NV        0x3375
#define EGL_STREAM_IMAGE_AVAILABLE_NV     0x3376
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
typedef EGLint (EGLAPIENTRYP PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMACQUIREIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMRELEASEIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#ifdef EGL_EGLEXT_PROTOTYPES
EGLAPI EGLBoolean EGLAPIENTRY eglStreamImageConsumerConnectNV (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
EGLAPI EGLint EGLAPIENTRY eglQueryStreamConsumerEventNV (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamAcquireImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamReleaseImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#endif
#endif

#ifndef EGL_EXT_device_drm
#define EGL_DRM_MASTER_FD_EXT                   0x333C
#endif

#ifndef EGL_EXT_device_drm_render_node
#define EGL_DRM_RENDER_NODE_FILE_EXT      0x3377
#endif

#ifndef EGL_NV_stream_reset
#define EGL_SUPPORT_REUSE_NV              0x3335
#endif

static PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
static PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
static PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;
static PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR;
static PFNEGLDESTROYSTREAMKHRPROC eglDestroyStreamKHR;
static PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV;

static void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s: %s", command, message);
}

void releaseExporter(NVDriver *drv) {
    //TODO not sure if this is still needed as we don't return anything now
    LOG("Releasing exporter, %d outstanding frames", drv->numFramesPresented);
    while (true) {
      CUeglFrame eglframe;
      CUresult cuStatus = drv->cu->cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_SUCCESS) {
        drv->numFramesPresented--;
        for (int i = 0; i < 3; i++) {
            if (eglframe.frame.pArray[i] != NULL) {
                LOG("Cleaning up CUDA array %p (%d outstanding)", eglframe.frame.pArray[i], drv->numFramesPresented);
                drv->cu->cuArrayDestroy(eglframe.frame.pArray[i]);
                eglframe.frame.pArray[i] = NULL;
            }
        }
      } else {
          break;
      }
    }
    LOG("Done releasing frames");

    if (drv->cuStreamConnection != NULL) {
        drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection);
    }

    if (drv->eglDisplay != EGL_NO_DISPLAY) {
        if (drv->eglStream != EGL_NO_STREAM_KHR) {
            eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
            drv->eglStream = EGL_NO_STREAM_KHR;
        }
        //TODO terminate the EGLDisplay here?, sounds like that could break stuff
        drv->eglDisplay = EGL_NO_DISPLAY;
    }
}

static void reconnect(NVDriver *drv) {
    LOG("Reconnecting to stream");
    eglInitialize(drv->eglDisplay, NULL, NULL);
    if (drv->cuStreamConnection != NULL) {
        CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerDisconnect(&drv->cuStreamConnection));
    }
    if (drv->eglStream != EGL_NO_STREAM_KHR) {
        eglDestroyStreamKHR(drv->eglDisplay, drv->eglStream);
    }
    //tell the driver we don't want it to reuse any EGLImages
    EGLint stream_attrib_list[] = { EGL_SUPPORT_REUSE_NV, EGL_FALSE, EGL_NONE };
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, stream_attrib_list);
    if (drv->eglStream == EGL_NO_STREAM_KHR) {
        LOG("Unable to create EGLStream");
        return;
    }
    if (!eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, NULL)) {
        LOG("Unable to connect EGLImage stream consumer");
        return;
    }
    CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));
    drv->numFramesPresented = 0;
}

static bool checkModesetParameterFromFd(int fd) {
    if (fd > 0) {
        //this ioctl should fail if modeset=0
        struct drm_get_cap caps = { .capability = DRM_CAP_DUMB_BUFFER };
        int ret = ioctl(fd, DRM_IOCTL_GET_CAP, &caps);
        if (ret != 0) {
            //the modeset parameter is set to 0
            LOG("ERROR: This driver requires the nvidia_drm.modeset kernel module parameter set to 1");
            return false;
        }
    } else {
        LOG("Unable to check nvidia_drm modeset setting");
    }
    return true;
}

int findGPUIndexFromFd(int displayType, int fd, int gpu, void **device) {
    struct stat buf;
    int drmDeviceIndex = -1;

    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLQUERYDEVICEATTRIBEXTPROC eglQueryDeviceAttribEXT = (PFNEGLQUERYDEVICEATTRIBEXTPROC) eglGetProcAddress("eglQueryDeviceAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");

    if (eglQueryDevicesEXT == NULL || eglQueryDeviceAttribEXT == NULL) {
        LOG("No support for EGL_EXT_device_enumeration");
        return 0;
    }

    //work out how we're searching for the GPU
    *device = NULL;
    if (gpu == -1 && fd != -1) {
        //figure out the 'drm device index', basically the minor number of the device node & 0x7f
        //since we don't know/what to care if we're dealing with a master or render node

        fstat(fd, &buf);
        drmDeviceIndex = minor(buf.st_rdev) & 0x7f;
        LOG("Looking for DRM device index: %d", drmDeviceIndex);
    } else {
        LOG("Looking for GPU index: %d", gpu);
    }

    //go grab some EGL devices
    EGLDeviceEXT devices[8];
    EGLint num_devices;
    if(!eglQueryDevicesEXT(8, devices, &num_devices)) {
        LOG("Unable to query EGL devices");
        return 0;
    }

    LOG("Found %d EGL devices", num_devices);
    for (int i = 0; i < num_devices; i++) {
        EGLAttrib attr = -1;

        //retrieve the DRM device path for this EGLDevice
        const char* drmDeviceFile = eglQueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
        if (drmDeviceFile != NULL) {
            //if we have one, try and get the CUDA device id
            if (eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &attr)) {
                LOG("Got EGL_CUDA_DEVICE_NV value '%d' for EGLDevice %d", attr, i);

                //if we're looking for a matching drm device index check it here
                if (gpu == -1 && fd != -1) {
                    stat(drmDeviceFile, &buf);
                    int foundDrmDeviceIndex = minor(buf.st_rdev) & 0x7f;
                    LOG("Found drmDeviceIndex: %d", foundDrmDeviceIndex);
                    if (foundDrmDeviceIndex != drmDeviceIndex) {
                        continue;
                    }
                } else if (gpu != attr) {
                    //LOG("Not selected device, skipping");
                    continue;
                }

                //if it's the device we're looking for, check the modeset parameter on it.
                if  (!checkModesetParameterFromFd(fd)) {
                    continue;
                }

                //TODO it's likely if we get here with (gpu != -1 && foundDrmDeviceIndex != drmDeviceIndex)
                //then the fd that was passed to us is not an NVIDIA GPU and we should try to implement some sort of optimus support
                //We can't really rely on the return from checking EGL_CUDA_DEVICE_NV as some non-NVIDIA drivers claim they support it

                LOG("Selecting EGLDevice %d", i);
                *device = devices[i];
                return attr;
            } else {
                LOG("No EGL_CUDA_DEVICE_NV support for EGLDevice %d", i);
            }
        } else {
            LOG("No DRM device file for EGLDevice %d", i);
        }
    }
    LOG("No match found, falling back to default device");
    return 0;
}

bool initExporter(NVDriver *drv, void *device) {
    static const EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};

    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    eglDestroyStreamKHR = (PFNEGLDESTROYSTREAMKHRPROC) eglGetProcAddress("eglDestroyStreamKHR");
    eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");

    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = (PFNEGLQUERYDMABUFFORMATSEXTPROC) eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");

    drv->eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, (EGLDeviceEXT) device, NULL);
    if (drv->eglDisplay == NULL) {
        LOG("Falling back to using default EGLDisplay");
        drv->eglDisplay = eglGetDisplay(NULL);
    }

    if (drv->eglDisplay == NULL) {
        return false;
    }

    if (!eglInitialize(drv->eglDisplay, NULL, NULL)) {
        LOG("Unable to initialise EGL for display: %p", drv->eglDisplay);
        return false;
    }
    //setup debug logging
    eglDebugMessageControlKHR(debug, debugAttribs);

    //see if the driver supports 16-bit exports
    EGLint formats[64];
    EGLint formatCount;
    if (eglQueryDmaBufFormatsEXT(drv->eglDisplay, 64, formats, &formatCount)) {
        bool r16 = false, rg1616 = false;
        for (int i = 0; i < formatCount; i++) {
            const char *fourcc = (const char *)&formats[i];
            LOG("Found format: %c%c%c%c", fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
            if (formats[i] == DRM_FORMAT_R16) {
                r16 = true;
            } else if (formats[i] == DRM_FORMAT_RG1616) {
                rg1616 = true;
            }
        }
        drv->supports16BitSurface = r16 & rg1616;
        if (drv->supports16BitSurface) {
            LOG("Driver supports 16-bit surfaces");
        } else {
            LOG("Driver doesn't support 16-bit surfaces");
        }
    }

    reconnect(drv);

    return true;
}

bool exportBackingImage(NVDriver *drv, BackingImage *img) {
    int planes = 0;
    if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, img->image, &img->fourcc, &planes, img->mods)) {
        LOG("eglExportDMABUFImageQueryMESA failed");
        return false;
    }

    LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx", img, (char*)&img->fourcc, img->fourcc, planes, img->mods[0], img->mods[1]);
    EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, img->image, img->fds, img->strides, img->offsets);
    //LOG("Offset/Pitch: %d %d %d %d", surface->offsets[0], surface->offsets[1], surface->strides[0], surface->strides[1]);

    if (!r) {
        LOG("Unable to export image");
        return false;
    }
    return true;
}

BackingImage* createBackingImage(NVDriver *drv, uint32_t width, uint32_t height, EGLImage image, CUarray arrays[]) {
    BackingImage* img = (BackingImage*) calloc(1, sizeof(BackingImage));
    img->image = image;
    img->arrays[0] = arrays[0];
    img->arrays[1] = arrays[1];
    img->width = width;
    img->height = height;

    if (!exportBackingImage(drv, img)) {
        LOG("Unable to export Backing Image");
        free(img);
        return NULL;
    }

    return img;
}


void destroyBackingImage(NVDriver *drv, BackingImage *img) {
    //if we're attached to a surface, update the surface to remove us
    if (img->surface != NULL) {
        img->surface->backingImage = NULL;
    }

    LOG("Destroying BackingImage: %p", img);
    for (int i = 0; i < 4; i++) {
        if (img->fds[i] != 0) {
            close(img->fds[i]);
        }
    }
    //eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, surface->eglImage, EGL_NO_SYNC);
    //destroy them rather than releasing them
    eglDestroyImage(drv->eglDisplay, img->image);
    CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[0]));
    CHECK_CUDA_RESULT(drv->cu->cuArrayDestroy(img->arrays[1]));
    img->arrays[0] = NULL;
    img->arrays[1] = NULL;
    free(img);
}

void attachBackingImageToSurface(NVSurface *surface, BackingImage *img) {
    surface->backingImage = img;
    img->surface = surface;
}

void detachBackingImageFromSurface(NVDriver *drv, NVSurface *surface) {
    if (surface->backingImage == NULL) {
        LOG("Cannot detach NULL BackingImage from Surface");
        return;
    }

    if (surface->backingImage->fourcc == DRM_FORMAT_NV21) {
        destroyBackingImage(drv, surface->backingImage);
    } else {
        pthread_mutex_lock(&drv->imagesMutex);

        ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
            //find the entry for this surface
            if (img->surface == surface) {
                LOG("Detaching BackingImage %p from Surface %p", img, surface);
                img->surface = NULL;
                break;
            }
        }

        pthread_mutex_unlock(&drv->imagesMutex);
    }

    surface->backingImage = NULL;
}

void destroyAllBackingImage(NVDriver *drv) {
    pthread_mutex_lock(&drv->imagesMutex);

    ARRAY_FOR_EACH_REV(BackingImage*, it, &drv->images)
        destroyBackingImage(drv, it);
        remove_element_at(&drv->images, it_idx);
    END_FOR_EACH

    pthread_mutex_unlock(&drv->imagesMutex);
}

BackingImage* findFreeBackingImage(NVDriver *drv, NVSurface *surface) {
    BackingImage *ret = NULL;
    pthread_mutex_lock(&drv->imagesMutex);
    //look through the free'd surfaces and see if we can reuse one
    ARRAY_FOR_EACH(BackingImage*, img, &drv->images)
        if (img->surface == NULL && img->width == surface->width && img->height == surface->height) {
            LOG("Using BackingImage %p for Surface %p", img, surface);
            attachBackingImageToSurface(surface, img);
            ret = img;
            break;
        }
    END_FOR_EACH
    pthread_mutex_unlock(&drv->imagesMutex);
    return ret;
}


BackingImage *allocateBackingImage(NVDriver *drv, const NVSurface *surface) {
    CUeglFrame eglframe = {
        .width = surface->width,
        .height = surface->height,
        .depth = 1,
        .pitch = 0,
        .planeCount = 2,
        .numChannels = 1,
        .frameType = CU_EGL_FRAME_TYPE_ARRAY,
    };

    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = drv->useCorrectNV12Format ? CU_EGL_COLOR_FORMAT_YUV420_SEMIPLANAR :
                                                              CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        if (surface->bitDepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitDepth == 12) {
            // Logically, we should use the explicit 12bit format here, but it fails
            // to export to a dmabuf if we do. In practice, that should be fine as the
            // data is still stored in 16 bits and they (surely?) aren't going to
            // zero out the extra bits.
            // eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
    }
    CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
        .Width = eglframe.width,
        .Height = eglframe.height,
        .Depth = 0,
        .NumChannels = 1,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
        .Width = eglframe.width >> 1,
        .Height = eglframe.height >> 1,
        .Depth = 0,
        .NumChannels = 2,
        .Flags = 0,
        .Format = eglframe.cuFormat
    };
    CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc));
    CHECK_CUDA_RESULT(drv->cu->cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc));

    pthread_mutex_lock(&drv->exportMutex);

    LOG("Presenting frame %d %dx%d (%p, %p, %p)", surface->pictureIdx, eglframe.width, eglframe.height, surface, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    CUresult result = drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL );
    if (result == CUDA_ERROR_UNKNOWN) {
        reconnect(drv);
        CHECK_CUDA_RESULT(drv->cu->cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL ));
    }

    BackingImage *ret = NULL;
    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        //check for the next event
        if (eglQueryStreamConsumerEventNV(drv->eglDisplay, drv->eglStream, 0, &event, &aux) != EGL_TRUE) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(drv->eglDisplay, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, drv->eglStream, NULL);
            LOG("Adding frame from EGLStream: %p", image);
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            //Not sure if this is ever called
            eglDestroyImage(drv->eglDisplay, (EGLImage) aux);
            LOG("Removing frame from EGLStream: %p", aux);
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed");
                break;
            }
            LOG("Acquired image from EGLStream: %p", img);

            ret = createBackingImage(drv, surface->width, surface->height, img, eglframe.frame.pArray);
        } else {
            LOG("Unhandled event: %X", event);
        }
    }

    pthread_mutex_unlock(&drv->exportMutex);
    return ret;
}

bool copyFrameToSurface(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    int bpp = surface->format == cudaVideoSurfaceFormat_NV12 ? 1 : 2;
    CUDA_MEMCPY2D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[0],
        .Height = surface->height,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2DAsync(&cpy, 0));
    CUDA_MEMCPY2D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = surface->height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = surface->backingImage->arrays[1],
        .Height = surface->height >> 1,
        .WidthInBytes = surface->width * bpp
    };
    CHECK_CUDA_RESULT(drv->cu->cuMemcpy2D(&cpy2));

    //notify anyone waiting for us to be resolved
    pthread_mutex_lock(&surface->mutex);
    surface->resolving = 0;
    pthread_cond_signal(&surface->cond);
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

bool realiseSurface(NVDriver *drv, NVSurface *surface) {
    //make sure we're the only thread updating this surface
    pthread_mutex_lock(&surface->mutex);
    //check again to see if it's just been created
    if (surface->backingImage == NULL) {
        //try to find a free surface
        BackingImage *img = findFreeBackingImage(drv, surface);

        //if we can't find a free backing image...
        if (img == NULL) {
            LOG("No free surfaces found");

            //...allocate one
            img = allocateBackingImage(drv, surface);
            if (img == NULL) {
                LOG("Unable to realise surface: %p (%d)", surface, surface->pictureIdx)
                pthread_mutex_unlock(&surface->mutex);
                return false;
            }

            if (img->fourcc == DRM_FORMAT_NV21) {
                LOG("Detected NV12/NV21 NVIDIA driver bug, attempting to work around");
                //free the old surface to prevent leaking them
                destroyBackingImage(drv, img);
                //this is a caused by a bug in old versions the driver that was fixed in the 510 series
                drv->useCorrectNV12Format = !drv->useCorrectNV12Format;
                //re-export the frame in the correct format
                img = allocateBackingImage(drv, surface);
                if (img->fourcc != DRM_FORMAT_NV12) {
                    LOG("Work around unsuccessful");
                }
            }

            attachBackingImageToSurface(surface, img);
            //add our newly created BackingImage to the list
            pthread_mutex_lock(&drv->imagesMutex);
            add_element(&drv->images, img);
            pthread_mutex_unlock(&drv->imagesMutex);
        }
    }
    pthread_mutex_unlock(&surface->mutex);

    return true;
}

bool exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch) {
    if (!realiseSurface(drv, surface)) {
        return false;
    }

    if (ptr != 0 && !copyFrameToSurface(drv, ptr, surface, pitch)) {
        LOG("Unable to update surface from frame");
        return false;
    } else if (ptr == 0) {
        LOG("exporting with null ptr");
    }

    return true;
}
