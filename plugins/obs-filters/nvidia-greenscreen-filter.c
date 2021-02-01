#include <obs-module.h>
#include <util/threading.h>
#include "nvvfx-load.h"
#include <pthread.h>
#include <cuda_runtime_api.h>

/* -------------------------------------------------------- */

#define do_log(level, format, ...)                            \
	blog(level, "[nvidia RTX greenscreen: '%s'] " format, \
	     obs_source_get_name(filter->context), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)

#ifdef _DEBUG
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)
#else
#define debug(format, ...)
#endif

/* -------------------------------------------------------- */
#define S_MODE "mode"
#define S_MODE_QUALITY 0
#define S_MODE_PERF 1

#define MT_ obs_module_text
#define TEXT_MODE MT_("Greenscreen.Mode")
#define TEXT_MODE_QUALITY MT_("Greenscreen.Quality")
#define TEXT_MODE_PERF MT_("Greenscreen.Performance")

bool nvvfx_loaded = false;

struct nv_greenscreen_data {
	obs_source_t *context;
	/* RTX SDK vars*/
	NvVFX_Handle handle;
	CUstream stream;             // cuda stream
	int mode;                    // 0 = quality, 1 = performance
	NvCVImage *src_img;          // src img in obs format on CPU
	NvCVImage *GPU_src_img;      // src img in obs format on GPU
	NvCVImage *BGR_src_img;      // src img in BGR on GPU
	NvCVImage *BGRA_CPU_src_img; // src img in BGR on GPU
	NvCVImage *A_dst_img;        // mask == output by nv greenscreen fx
	NvCVImage *A_CPU_dst_img;    // mask on CPU
	bool images_allocated;

	enum video_format current_format; // format of obs frame
	enum video_format new_format;     // new format of obs frame
	bool tick_flag;                   // only render at new frames

	/* alpha mask effect */
	gs_effect_t *effect;
	gs_texture_t *texture;
	gs_texture_t *rgba_texture;
	gs_eparam_t *mask_param;
	gs_eparam_t *rgba_param;
	uint32_t mask_width;

	/* processing thread */
	bool nvvfx_thread_active;
	pthread_t nvvfx_thread;
	pthread_mutex_t nvvfx_mutex;
	os_sem_t *nvvfx_sem;
	DARRAY(struct obs_source_frame *) frames;
};

enum NvCVImage_PixelFormat
obs_video_format_to_nvidia_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		return NVCV_YUV420;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_I422:
		return NVCV_YUV422;
	case VIDEO_FORMAT_RGBA:
		return NVCV_RGBA;
	case VIDEO_FORMAT_BGRA:
		return NVCV_BGRA;
	case VIDEO_FORMAT_Y800:
		return NVCV_Y;
	case VIDEO_FORMAT_BGR3:
		return NVCV_BGR;
	case VIDEO_FORMAT_I444:
		return NVCV_YUV444;
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_NONE:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_BGRX:
		/* not supported by nv fx */
		return NVCV_FORMAT_UNKNOWN;
	}
}

static const char *nv_greenscreen_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NvidiaGreenscreenFilter");
}

static void nv_greenscreen_filter_update(void *data, obs_data_t *settings)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	NvCV_Status vfxErr;
	int mode = obs_data_get_int(settings, S_MODE);
	filter->mode = mode;
	const char *modetxt = mode ? "performance" : "quality";
	vfxErr = NvVFX_SetU32(filter->handle, NVVFX_MODE, mode);
	vfxErr = NvVFX_Load(filter->handle);
	if (NVCV_SUCCESS != vfxErr)
		error("Error loading FX %i", vfxErr);
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4706)
#endif

bool load_nvvfx(void)
{
	if (!load_nv_vfx_lib()) {
		blog(LOG_INFO,
		     "[nvidia RTX greenscreen]: NVIDIA RTX Video FX disabled, redistributable not found");
		return false;
	}

#define LOAD_SYM_FROM_LIB(sym, lib, dll)                               \
	if (!(sym = (sym##_t)GetProcAddress(lib, #sym))) {             \
		DWORD err = GetLastError();                            \
		printf("[nvidia RTX greenscreen]: Couldn't load " #sym \
		       " from " dll ": %lu (0x%lx)",                   \
		       err, err);                                      \
		release_nv_vfx_lib();                                  \
		goto unload_everything;                                \
	}

#define LOAD_SYM(sym) LOAD_SYM_FROM_LIB(sym, nv_videofx, "NVVideoEffects.dll")
	LOAD_SYM(NvVFX_GetVersion);
	LOAD_SYM(NvVFX_CreateEffect);
	LOAD_SYM(NvVFX_DestroyEffect);
	LOAD_SYM(NvVFX_SetU32);
	LOAD_SYM(NvVFX_SetS32);
	LOAD_SYM(NvVFX_SetF32);
	LOAD_SYM(NvVFX_SetF64);
	LOAD_SYM(NvVFX_SetU64);
	LOAD_SYM(NvVFX_SetObject);
	LOAD_SYM(NvVFX_SetCudaStream);
	LOAD_SYM(NvVFX_SetImage);
	LOAD_SYM(NvVFX_SetString);
	LOAD_SYM(NvVFX_GetU32);
	LOAD_SYM(NvVFX_GetS32);
	LOAD_SYM(NvVFX_GetF32);
	LOAD_SYM(NvVFX_GetF64);
	LOAD_SYM(NvVFX_GetU64);
	LOAD_SYM(NvVFX_GetObject);
	LOAD_SYM(NvVFX_GetCudaStream);
	LOAD_SYM(NvVFX_GetImage);
	LOAD_SYM(NvVFX_GetString);
	LOAD_SYM(NvVFX_Run);
	LOAD_SYM(NvVFX_Load);
	LOAD_SYM(NvVFX_CudaStreamCreate);
	LOAD_SYM(NvVFX_CudaStreamDestroy);
#undef LOAD_SYM
#define LOAD_SYM(sym) LOAD_SYM_FROM_LIB(sym, nv_cvimage, "NVCVImage.dll")
	LOAD_SYM(NvCV_GetErrorStringFromCode);
	LOAD_SYM(NvCVImage_Init);
	LOAD_SYM(NvCVImage_InitView);
	LOAD_SYM(NvCVImage_Alloc);
	LOAD_SYM(NvCVImage_Realloc);
	LOAD_SYM(NvCVImage_Dealloc);
	LOAD_SYM(NvCVImage_Create);
	LOAD_SYM(NvCVImage_Destroy);
	LOAD_SYM(NvCVImage_ComponentOffsets);
	LOAD_SYM(NvCVImage_Transfer);
	LOAD_SYM(NvCVImage_TransferRect);
	LOAD_SYM(NvCVImage_TransferFromYUV);
	LOAD_SYM(NvCVImage_TransferToYUV);
	LOAD_SYM(NvCVImage_MapResource);
	LOAD_SYM(NvCVImage_UnmapResource);
	LOAD_SYM(NvCVImage_Composite);
	LOAD_SYM(NvCVImage_CompositeRect);
	LOAD_SYM(NvCVImage_CompositeOverConstant);
	LOAD_SYM(NvCVImage_FlipY);
	LOAD_SYM(NvCVImage_GetYUVPointers);
	LOAD_SYM(NvCVImage_InitFromD3D11Texture);
	LOAD_SYM(NvCVImage_ToD3DFormat);
	LOAD_SYM(NvCVImage_FromD3DFormat);
	LOAD_SYM(NvCVImage_ToD3DColorSpace);
	LOAD_SYM(NvCVImage_FromD3DColorSpace);
#undef LOAD_SYM

	int err;
	NvVFX_Handle h = NULL;

	/* load the effect to check if the GPU is supported */
	err = NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &h);
	if (err != NVCV_SUCCESS) {
		if (err == NVCV_ERR_UNSUPPORTEDGPU) {
			blog(LOG_INFO,
			     "[nvidia RTX greenscreen]: NVIDIA RTX Greenscreen disabled: unsupported GPU");
		} else {
			blog(LOG_ERROR,
			     "[nvidia RTX greenscreen]: NVIDIA RTX Greenscreen disabled: error %i",
			     err);
		}
		goto unload_everything;
	}
	NvVFX_DestroyEffect(h);

	nvvfx_loaded = true;
	blog(LOG_INFO,
	     "[nvidia RTX greenscreen]: NVIDIA RTX Greenscreen enabled, redistributable found");
	return true;

unload_everything:
	nvvfx_loaded = false;
	release_nv_vfx_lib();
	return false;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

void unload_nvvfx(void)
{
	release_nv_vfx_lib();
}

static void nv_greenscreen_filter_destroy(void *data)
{
	if (!nvvfx_loaded)
		return;
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	pthread_mutex_lock(&filter->nvvfx_mutex);
	filter->nvvfx_thread_active = false;
	pthread_mutex_unlock(&filter->nvvfx_mutex);
	NvCVImage_Destroy(filter->src_img);
	NvCVImage_Destroy(filter->GPU_src_img);
	NvCVImage_Destroy(filter->BGR_src_img);
	NvCVImage_Destroy(filter->BGRA_CPU_src_img);
	NvCVImage_Destroy(filter->A_dst_img);
	NvCVImage_Destroy(filter->A_CPU_dst_img);
	NvVFX_CudaStreamDestroy(filter->stream);
	if (filter->handle) {
		NvVFX_DestroyEffect(filter->handle);
	}

	os_sem_post(filter->nvvfx_sem);
	pthread_join(filter->nvvfx_thread, NULL);
	pthread_mutex_destroy(&filter->nvvfx_mutex);
	os_sem_destroy(filter->nvvfx_sem);
	for (size_t i = 0; i < filter->frames.num; i++)
		free(filter->frames.array + i);
	da_free(filter->frames);

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		gs_texture_destroy(filter->texture);
		gs_texture_destroy(filter->rgba_texture);
		obs_leave_graphics();
	}
	bfree(filter);
}

static int process_frame(struct nv_greenscreen_data *data)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	struct obs_source_frame *frame;
	NvCV_Status vfxErr;
	bool new_frame = false;
	pthread_mutex_lock(&filter->nvvfx_mutex);
	if (filter->frames.num) {
		frame = filter->frames.array[0];
		da_erase(filter->frames, 0);
		new_frame = true;
	}
	pthread_mutex_unlock(&filter->nvvfx_mutex);

	if (!new_frame)
		return false;
	enum NvCVImage_PixelFormat format =
		obs_video_format_to_nvidia_format(frame->format);
	int layout = get_nv_layout_from_obs_format(frame->format);
	unsigned int width = (unsigned int)frame->width;
	unsigned int height = (unsigned int)frame->height;
	unsigned char colorspace =
		frame->full_range
			? NVCV_709 | NVCV_FULL_RANGE | NVCV_CHROMA_COSITED
			: NVCV_709 | NVCV_VIDEO_RANGE | NVCV_CHROMA_COSITED;

	/* 1. Pass obs video data to an NvCVImage */
	vfxErr = NvCVImage_Init(filter->src_img, width, height,
				filter->src_img->pitch, frame->data[0], format,
				NVCV_U8, layout, NVCV_CPU);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error passing obs frame data to NvCVImage on CPU, error %i",
		      vfxErr);
		return false;
	}

	/* 2. Transfer video data to GPU */
	vfxErr = NvCVImage_Transfer(filter->src_img, filter->GPU_src_img, 1.0f,
				    filter->stream, NULL);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error converting to NVCVImage on GPU , error %i",
		      vfxErr);
		return false;
	}

	/* 3. Convert video data to BGR on GPU */
	vfxErr = NvCVImage_Transfer(filter->GPU_src_img, filter->BGR_src_img,
				    1.0f, filter->stream, NULL);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error converting to BGR , error %i", vfxErr);
		return false;
	}
	vfxErr = NvCVImage_Transfer(filter->BGR_src_img,
				    filter->BGRA_CPU_src_img, 1.0f,
				    filter->stream, NULL);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error converting to BGR , error %i", vfxErr);
		return false;
	}
	/*  4. run RTX fx */
	vfxErr = NvVFX_Run(filter->handle, 1);
	if (vfxErr != NVCV_SUCCESS) {
		const char *string = NvCV_GetErrorStringFromCode(vfxErr);
		error("Error running the FX, error %i, message : %s", vfxErr,
		      string);
		return false;
	}

	/* 5. Copy alpha to CPU */
	vfxErr = NvCVImage_Transfer(filter->A_dst_img, filter->A_CPU_dst_img,
				    1.0f, filter->stream, NULL);
	if (vfxErr != NVCV_SUCCESS)
		error("Error downloading alpha to CPU, error %i, ", vfxErr);

	return true;
}

static void nvvfx_process(void *data)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;

	while (os_sem_wait(filter->nvvfx_sem) == 0 &&
	       filter->nvvfx_thread_active) {
		if (!process_frame(filter))
			return;
	}
	return;
}

static void *nv_greenscreen_filter_create(obs_data_t *settings,
					  obs_source_t *context)
{
	struct nv_greenscreen_data *filter =
		(struct nv_greenscreen_data *)bzalloc(sizeof(*filter));
	if (!nvvfx_loaded)
		nv_greenscreen_filter_destroy(filter);
	NvCV_Status vfxErr;
	filter->mode = 0;
	filter->images_allocated = false;
	filter->tick_flag = false;

	vfxErr = NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &filter->handle);
	if (NVCV_SUCCESS != vfxErr) {
		error("Error creating effect, error %i", vfxErr);
	}

	char buffer[MAX_PATH];
	char modelDir[MAX_PATH];
	nvvfx_get_sdk_path(buffer, MAX_PATH);
	size_t max_len = sizeof(buffer) / sizeof(char);
	snprintf(modelDir, max_len, "%s\\models", buffer);
	vfxErr = NvVFX_SetString(filter->handle, NVVFX_MODEL_DIRECTORY,
				 modelDir);
	vfxErr = NvVFX_CudaStreamCreate(&filter->stream);
	if (NVCV_SUCCESS != vfxErr) {
		error("Error creating CUDA Stream %i", vfxErr);
	}
	vfxErr = NvVFX_SetCudaStream(filter->handle, NVVFX_CUDA_STREAM,
				     filter->stream);

	/* load alpha mask effect */
	char *effect_path = obs_module_file("rtx_greenscreen.effect");

	filter->context = context;

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);
	if (filter->effect) {
		filter->mask_param =
			gs_effect_get_param_by_name(filter->effect, "mask");
		filter->rgba_param =
			gs_effect_get_param_by_name(filter->effect, "image");
	}
	obs_leave_graphics();

	if (!filter->effect) {
		nv_greenscreen_filter_destroy(filter);
		return NULL;
	}

	/* processing thread */
	int ret = pthread_create(&filter->nvvfx_thread, NULL, nvvfx_process,
				 filter);
	if (ret != 0) {
		error("failed to create processing thread.");
		nv_greenscreen_filter_destroy(filter);
		return NULL;
	}
	filter->nvvfx_thread_active = true;
	if (pthread_mutex_init(&filter->nvvfx_mutex, NULL) != 0 ||
	    os_sem_init(&filter->nvvfx_sem, 0) != 0) {
		nv_greenscreen_filter_destroy(filter);
		return NULL;
	}

	/*---------------------------------------- */

	nv_greenscreen_filter_update(filter, settings);

	return filter;
}

static obs_properties_t *nv_greenscreen_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, S_MODE, TEXT_MODE,
						       OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, TEXT_MODE_QUALITY, S_MODE_QUALITY);
	obs_property_list_add_int(mode, TEXT_MODE_PERF, S_MODE_PERF);
	UNUSED_PARAMETER(data);
	return props;
}

int get_nv_layout_from_obs_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return NVCV_I420;
	case VIDEO_FORMAT_NV12:
		return NVCV_NV12;
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I422:
		return NVCV_PLANAR;
	case VIDEO_FORMAT_YVYU:
		return NVCV_YVYU;
	case VIDEO_FORMAT_YUY2:
		return NVCV_YUY2;
	case VIDEO_FORMAT_UYVY:
		return NVCV_UYVY;
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_AYUV:
		return NVCV_INTERLEAVED;
	}
}

static inline bool is_420_422_yuv(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
		return true;
	}
	return false;
}

int format_plane_numbers(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NONE:
		return 0;
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_AYUV:
		return 1;
	case VIDEO_FORMAT_NV12:
		return 2;
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I444:
		return 3;
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
		return 4;
	}
}

static struct obs_source_frame *
nv_greenscreen_filter_video(void *data, struct obs_source_frame *frame)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	filter->new_format = frame->format;
	NvCV_Status vfxErr;

	/* minimum size supported by SDK is (512,288) */
	if (frame->width < 512 || frame->height < 288) {
		error("Size must be larger than (512,288)");
		return frame;
	}

	/* I40A, I42A, YUVA not supported by the SDK */
	int planes = format_plane_numbers(frame->format);
	if (planes == 4) {
		error("Video format not supported by RTX VFX SDK");
		return frame;
	}
	/* Make sure to reallocate images if format is changed (eg. for a webcam). */
	if (filter->current_format != filter->new_format) {
		filter->images_allocated = false;
		filter->current_format = frame->format;
	}
	pthread_mutex_lock(&filter->nvvfx_mutex);
	/* 1. Initial allocations of NvCVImages used by data struct */
	if (!filter->images_allocated) {
		unsigned int width = (unsigned int)frame->width;
		unsigned int height = (unsigned int)frame->height;
		enum NvCVImage_PixelFormat format =
			obs_video_format_to_nvidia_format(frame->format);
		int layout = get_nv_layout_from_obs_format(frame->format);
		unsigned char colorspace =
			frame->full_range ? NVCV_709 | NVCV_FULL_RANGE |
						    NVCV_CHROMA_COSITED
					  : NVCV_709 | NVCV_VIDEO_RANGE |
						    NVCV_CHROMA_COSITED;
		//set width of mask
		filter->mask_width = width;

		// create alpha & rgba texture
		filter->texture = gs_texture_create(width, height, GS_A8, 1,
						    NULL, GS_DYNAMIC);
		if (filter->texture == NULL) {
			error("Alpha texture couldn't be created");
		}
		filter->rgba_texture = gs_texture_create(width, height, GS_RGBA,
							 1, NULL, GS_DYNAMIC);
		if (filter->texture == NULL) {
			error("RGBA texture couldn't be created");
		}
		// create and allocate src image with obs params on CPU
		vfxErr = NvCVImage_Create(width, height, format, NVCV_U8,
					  layout, NVCV_CPU, 0,
					  &filter->src_img);
		if (is_420_422_yuv(frame->format))
			filter->src_img->colorspace = colorspace;

		// create and allocate image with obs params on GPU
		vfxErr = NvCVImage_Create(width, height, format, NVCV_U8,
					  layout, NVCV_GPU, 1,
					  &filter->GPU_src_img);
		if (is_420_422_yuv(frame->format))
			filter->GPU_src_img->colorspace = colorspace;
		vfxErr = NvCVImage_Alloc(filter->GPU_src_img, width, height,
					 format, NVCV_U8, layout, NVCV_GPU, 1);

		// create and allocate src image converted to BGR on GPU
		vfxErr = NvCVImage_Create(width, height, NVCV_BGR, NVCV_U8,
					  NVCV_CHUNKY, NVCV_GPU, 1,
					  &filter->BGR_src_img);
		vfxErr = NvCVImage_Alloc(filter->BGR_src_img, width, height,
					 NVCV_BGR, NVCV_U8, NVCV_CHUNKY,
					 NVCV_GPU, 1);
		// create and allocate src image converted to BGR on GPU
		vfxErr = NvCVImage_Create(width, height, NVCV_RGBA, NVCV_U8,
					  NVCV_CHUNKY, NVCV_CPU_PINNED, 1,
					  &filter->BGRA_CPU_src_img);
		vfxErr = NvCVImage_Alloc(filter->BGRA_CPU_src_img, width,
					 height, NVCV_RGBA, NVCV_U8,
					 NVCV_CHUNKY, NVCV_CPU_PINNED, 1);
		// create and allocate Alpha image from FX on GPU; output for fx
		vfxErr = NvCVImage_Create(width, height, NVCV_A, NVCV_U8,
					  NVCV_CHUNKY, NVCV_GPU, 1,
					  &filter->A_dst_img);
		vfxErr = NvCVImage_Alloc(filter->A_dst_img, width, height,
					 NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_GPU,
					 1);

		// create and allocate Alpha image from FX on CPU
		vfxErr = NvCVImage_Create(width, height, NVCV_A, NVCV_U8,
					  NVCV_CHUNKY, NVCV_CPU_PINNED, 1,
					  &filter->A_CPU_dst_img);
		vfxErr = NvCVImage_Alloc(filter->A_CPU_dst_img, width, height,
					 NVCV_A, NVCV_U8, NVCV_CHUNKY,
					 NVCV_CPU_PINNED, 1);

		// set input & output images for greenscreen fx
		vfxErr = NvVFX_SetImage(filter->handle, NVVFX_INPUT_IMAGE,
					filter->BGR_src_img);
		vfxErr = NvVFX_SetImage(filter->handle, NVVFX_OUTPUT_IMAGE,
					filter->A_dst_img);
		if (vfxErr != NVCV_SUCCESS) {
			error("Error during allocation of images, error %i",
			      vfxErr);
			return frame;
		}
		filter->images_allocated = true;
	}

	/* 2. Capture frame */
	da_push_back(filter->frames, &frame);
	pthread_mutex_unlock(&filter->nvvfx_mutex);
	os_sem_post(filter->nvvfx_sem);
	return frame;
}

static void nv_greenscreen_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MODE, S_MODE_QUALITY);
}

static void nv_greenscreen_filter_render(void *data, gs_effect_t *effect)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	uint32_t width = filter->mask_width;

	/* Skip if processing of a frame hasn't yet started */
	if (width == 0 || !filter->A_dst_img) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	filter->tick_flag = false;
	cudaError_t CUDARTAPI cudaErr = cudaStreamSynchronize(filter->stream);
	/* Set alpha mask to texture */
	obs_enter_graphics();
	gs_texture_set_image(filter->texture,
			     (uint8_t *)filter->BGRA_CPU_src_img->pixels,
			     4 * width, false);
	gs_texture_set_image(filter->texture,
			     (uint8_t *)filter->A_CPU_dst_img->pixels, width,
			     false);
	obs_leave_graphics();

	/* Render alpha mask */
	if (!obs_source_process_filter_begin(filter->context, GS_RGBA,
					     OBS_ALLOW_DIRECT_RENDERING))
		return;
	gs_effect_set_texture(filter->mask_param, filter->texture);
	gs_effect_set_texture(filter->rgba_param, filter->rgba_texture);
	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
	pthread_mutex_lock(&filter->nvvfx_mutex);

	pthread_mutex_unlock(&filter->nvvfx_mutex);
	UNUSED_PARAMETER(effect);
}

struct obs_source_info nvidia_greenscreen_filter_info = {
	.id = "nv_greenscreen_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = nv_greenscreen_filter_name,
	.create = nv_greenscreen_filter_create,
	.destroy = nv_greenscreen_filter_destroy,
	.get_defaults = nv_greenscreen_filter_defaults,
	.get_properties = nv_greenscreen_filter_properties,
	.update = nv_greenscreen_filter_update,
	.video_render = nv_greenscreen_filter_render,
	.filter_video = nv_greenscreen_filter_video,
};
