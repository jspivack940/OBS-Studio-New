#include <obs-module.h>
#include <util/threading.h>
#include <pthread.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include "nvvfx-load.h"
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
#define S_THRESHOLDFX "threshold"
#define S_THRESHOLDFX_DEFAULT 0.9

#define MT_ obs_module_text
#define TEXT_MODE MT_("Greenscreen.Mode")
#define TEXT_MODE_QUALITY MT_("Greenscreen.Quality")
#define TEXT_MODE_PERF MT_("Greenscreen.Performance")
#define TEXT_MODE_THRESHOLD MT_("Greenscreen.threshold")

bool nvvfx_loaded = false;
struct nv_greenscreen_data {
	obs_source_t *context;
	bool images_allocated;
	bool processing_stop;
	bool processed_frame;
	bool target_valid;
	int count;

	/* RTX SDK vars*/
	NvVFX_Handle handle;
	CUstream stream;        // cuda stream
	int mode;               // 0 = quality, 1 = performance
	NvCVImage *src_img;     // src img in obs format (RGBA ?) on GPU
	NvCVImage *BGR_src_img; // src img in BGR on GPU
	NvCVImage *A_dst_img;   // mask img on GPU
	NvCVImage *dst_img;     // mask texture
	NvCVImage *stage;       // planar stage img used for transfer to texture

	/* alpha mask effect */
	gs_effect_t *effect;
	gs_texrender_t *render;
	gs_texture_t *alpha_texture;
	uint32_t width;  // width of texture
	uint32_t height; // height of texture
	gs_eparam_t *mask_param;
	gs_texture_t *src_texture;
	gs_eparam_t *src_param;
	gs_eparam_t *threshold_param;
	double threshold;

	/* mutexes */
	pthread_mutex_t nvvfx_mutex;
};

static const char *nv_greenscreen_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NvidiaGreenscreenFilter");
}

static void nv_greenscreen_filter_update(void *data, obs_data_t *settings)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	NvCV_Status vfxErr;
	int mode = (int)obs_data_get_int(settings, S_MODE);
	if (filter->mode != mode) {
		filter->mode = mode;
		vfxErr = NvVFX_SetU32(filter->handle, NVVFX_MODE, mode);
		vfxErr = NvVFX_Load(filter->handle);
		if (NVCV_SUCCESS != vfxErr)
			error("Error loading Greenscreen FX %i", vfxErr);
	}
	filter->threshold =
		(double)obs_data_get_double(settings, S_THRESHOLDFX);
}

static void nv_greenscreen_filter_destroy(void *data)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	if (!nvvfx_loaded) {
		bfree(filter);
		return;
	}
	filter->processing_stop = true;

	if (filter->effect) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		gs_texture_destroy(filter->alpha_texture);
		gs_texrender_destroy(filter->render);
		obs_leave_graphics();
	}
	pthread_mutex_destroy(&filter->nvvfx_mutex);
	NvCVImage_Destroy(filter->src_img);
	NvCVImage_Destroy(filter->BGR_src_img);
	NvCVImage_Destroy(filter->A_dst_img);
	NvCVImage_Destroy(filter->dst_img);
	NvCVImage_Destroy(filter->stage);
	NvVFX_CudaStreamDestroy(filter->stream);
	if (filter->handle) {
		NvVFX_DestroyEffect(filter->handle);
	}
	bfree(filter);
}

static void init_images_gr(struct nv_greenscreen_data *filter)
{
	NvCV_Status vfxErr;
	uint32_t width = filter->width;
	uint32_t height = filter->height;

	/* 1. create alpha texture */
	obs_enter_graphics();
	if (filter->alpha_texture)
		gs_texture_destroy(filter->alpha_texture);
	filter->alpha_texture =
		gs_texture_create(width, height, GS_A8, 1, NULL, 0);
	if (filter->alpha_texture == NULL) {
		error("Alpha texture couldn't be created");
	}
	/* 2. create texrender */
	if (filter->render)
		gs_texrender_destroy(filter->render);
	filter->render = gs_texrender_create(GS_RGBA_UNORM, GS_ZS_NONE);
	obs_leave_graphics();

	/* 3. Create source NvCVImage. Allocation not required because we'll
	   pass the texture buffer */
	vfxErr = NvCVImage_Create(width, height, NVCV_RGBA, NVCV_U8,
				  NVCV_CHUNKY, NVCV_GPU, 1, &filter->src_img);
	if (vfxErr != NVCV_SUCCESS) {
		goto fail;
	}
	/* 4. Create and allocate BGR NvCVImage. */
	vfxErr = NvCVImage_Create(width, height, NVCV_BGR, NVCV_U8, NVCV_CHUNKY,
				  NVCV_GPU, 1, &filter->BGR_src_img);
	vfxErr = NvCVImage_Alloc(filter->BGR_src_img, width, height, NVCV_BGR,
				 NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
	if (vfxErr != NVCV_SUCCESS) {
		goto fail;
	}
	/* 5. Create and allocate Alpha NvCVimage on GPU. */
	vfxErr = NvCVImage_Create(width, height, NVCV_A, NVCV_U8, NVCV_CHUNKY,
				  NVCV_GPU, 1, &filter->A_dst_img);
	vfxErr = NvCVImage_Alloc(filter->A_dst_img, width, height, NVCV_A,
				 NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1);
	if (vfxErr != NVCV_SUCCESS) {
		goto fail;
	}

	/* 6.' Create NvCVImage which will hold final texture. */
	vfxErr = NvCVImage_Create(width, height, NVCV_A, NVCV_U8, NVCV_CHUNKY,
				  NVCV_GPU, 1, &filter->dst_img);

	/* 6.'' Create stage NvCVImage which will be used as buffer for transfer */
	vfxErr = NvCVImage_Create(width, height, NVCV_RGBA, NVCV_U8,
				  NVCV_PLANAR, NVCV_GPU, 1, &filter->stage);
	vfxErr = NvCVImage_Alloc(filter->stage, width, height, NVCV_RGBA,
				 NVCV_U8, NVCV_PLANAR, NVCV_GPU, 1);
	if (vfxErr != NVCV_SUCCESS) {
		goto fail;
	}
	/* 7. Set input & output images for nv FX. */
	vfxErr = NvVFX_SetImage(filter->handle, NVVFX_INPUT_IMAGE,
				filter->BGR_src_img);
	vfxErr = NvVFX_SetImage(filter->handle, NVVFX_OUTPUT_IMAGE,
				filter->A_dst_img);

	if (vfxErr != NVCV_SUCCESS) {
		goto fail;
	}
	filter->images_allocated = true;
	return;
fail:
	error("Error during allocation of images, error %i", vfxErr);
	nv_greenscreen_filter_destroy(filter);
}

static bool process_texture_gr(struct nv_greenscreen_data *filter)
{
	gs_texrender_t *render = filter->render;
	NvCV_Status vfxErr;

	/* 1. Retrieve d3d11texture2d from texrender */
	obs_enter_graphics();
	gs_texture_t *texture = gs_texrender_get_texture(render);
	enum gs_color_format color = gs_texture_get_color_format(texture);
	filter->src_texture = texture;
	struct ID3D11Texture2D *d11texture =
		(struct ID3D11Texture2D *)gs_texture_get_obj(texture);
	obs_leave_graphics();
	if (!d11texture) {
		error("Couldn't retrieve d3d11texture2d.");
		return false;
	}
	/* 2. Init NvCVImage with ID3D11Texure2D & map before transfer */
	vfxErr = NvCVImage_Dealloc(filter->src_img);
	vfxErr = NvCVImage_InitFromD3D11Texture(filter->src_img, d11texture);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error passing ID3D11Texture to NvCVImage, error %i",
		      vfxErr);
		pthread_mutex_lock(&filter->nvvfx_mutex);
		init_images_gr(filter); // non fatal error
		pthread_mutex_unlock(&filter->nvvfx_mutex);
		return false;
	}
	vfxErr = NvCVImage_MapResource(filter->src_img, filter->stream);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error mapping resource for source texture, error %i",
		      vfxErr);
		goto fail;
	}

	/* 3. Convert to BGR if neccessary */
	vfxErr = NvCVImage_Transfer(filter->src_img, filter->BGR_src_img, 1.0f,
				    filter->stream, filter->stage);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error converting to BGR NVCVImage on GPU , error %i",
		      vfxErr);
		goto fail;
	}
	vfxErr = NvCVImage_UnmapResource(filter->src_img, filter->stream);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error unmapping resource for source texture, error %i",
		      vfxErr);
		goto fail;
	}

	/*  4. run RTX fx */
	vfxErr = NvVFX_Run(filter->handle, 1);
	if (vfxErr != NVCV_SUCCESS) {
		const char *string = NvCV_GetErrorStringFromCode(vfxErr);
		error("Error running the FX, error %i, message : %s", vfxErr,
		      string);
		goto fail;
	}

	/* 5. Transfer to texture */
	obs_enter_graphics();
	struct ID3D11Texture2D *d11texture2 =
		(struct ID3D11Texture2D *)gs_texture_get_obj(
			filter->alpha_texture);
	obs_leave_graphics();
	vfxErr = NvCVImage_Dealloc(filter->dst_img);
	vfxErr = NvCVImage_InitFromD3D11Texture(filter->dst_img, d11texture2);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error passing final ID3D11Texture to NvCVImage, error %i",
		      vfxErr);
		pthread_mutex_lock(&filter->nvvfx_mutex);
		init_images_gr(filter); // non fatal error
		pthread_mutex_unlock(&filter->nvvfx_mutex);
		return false;
	}
	vfxErr = NvCVImage_MapResource(filter->dst_img, filter->stream);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error mapping resource for dst texture, error %i",
		      vfxErr);
		goto fail;
	}

	vfxErr = NvCVImage_Transfer(filter->A_dst_img, filter->dst_img, 1.0f,
				    filter->stream, filter->stage);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error transferring mask to alpha texture, error %i, ",
		      vfxErr);
		goto fail;
	}
	vfxErr = NvCVImage_UnmapResource(filter->dst_img, filter->stream);
	if (vfxErr != NVCV_SUCCESS) {
		error("Error unmapping resource for dst texture, error %i",
		      vfxErr);
		goto fail;
	}
	cudaError_t CUDARTAPI cudaErr = cudaStreamSynchronize(filter->stream);

	return true;
fail:
	nv_greenscreen_filter_destroy(filter);
	return false;
}

static void *nv_greenscreen_filter_create(obs_data_t *settings,
					  obs_source_t *context)
{
	struct nv_greenscreen_data *filter =
		(struct nv_greenscreen_data *)bzalloc(sizeof(*filter));
	if (!nvvfx_loaded)
		nv_greenscreen_filter_destroy(filter);
	NvCV_Status vfxErr;
	filter->mode =
		-1; // triggers initialization since it's not a valid value
	filter->images_allocated = false;
	filter->processed_frame =
		true; // processing starts when tick says processed_frame is false
	filter->width = 0;
	filter->height = 0;
	filter->count = 0;
	filter->processing_stop = false;

	/* Create FX */
	vfxErr = NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &filter->handle);
	if (NVCV_SUCCESS != vfxErr) {
		error("Error creating effect, error %i", vfxErr);
		nv_greenscreen_filter_destroy(filter);
	}
	/* Set models path & initialize CudaStream */
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
		nv_greenscreen_filter_destroy(filter);
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
		filter->src_param =
			gs_effect_get_param_by_name(filter->effect, "image");
		filter->threshold_param = gs_effect_get_param_by_name(
			filter->effect, "threshold");
	}
	obs_leave_graphics();

	if (!filter->effect) {
		nv_greenscreen_filter_destroy(filter);
		return NULL;
	}
	if (pthread_mutex_init(&filter->nvvfx_mutex, NULL) != 0) {
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
	obs_property_t *threshold = obs_properties_add_float_slider(
		props, S_THRESHOLDFX, TEXT_MODE_THRESHOLD, 0, 1, 0.05);
	UNUSED_PARAMETER(data);
	return props;
}

static void nv_greenscreen_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_MODE, S_MODE_QUALITY);
	obs_data_set_default_double(settings, S_THRESHOLDFX,
				    S_THRESHOLDFX_DEFAULT);
}

static void nv_greenscreen_filter_tick(void *data, float t)
{
	UNUSED_PARAMETER(t);

	struct nv_greenscreen_data *filter = data;
	if (filter->processing_stop)
		return;
	obs_source_t *target = obs_filter_get_target(filter->context);
	uint32_t cx;
	uint32_t cy;

	filter->target_valid = !!target;
	if (!filter->target_valid)
		return false;

	cx = obs_source_get_base_width(target);
	cy = obs_source_get_base_height(target);

	// initially the sizes are 0
	if (!cx && !cy) {
		filter->target_valid = false;
		return false;
	}

	/* minimum size supported by SDK is (512,288) */
	filter->target_valid = cx >= 512 && cy >= 288;
	if (!filter->target_valid) {
		error("Size must be larger than (512,288)");
		nv_greenscreen_filter_destroy(filter);
	}
	if (cx != filter->width && cy != filter->height) {
		filter->images_allocated = false;
		filter->width = cx;
		filter->height = cy;
	}
	if (!filter->images_allocated)
		init_images_gr(filter);

	filter->processed_frame = false;
}

static void draw_gr(struct nv_greenscreen_data *filter)
{

	/* Render alpha mask */
	if (!obs_source_process_filter_begin(filter->context, GS_RGBA_UNORM,
					     OBS_ALLOW_DIRECT_RENDERING))
		return;
	gs_effect_set_texture(filter->mask_param, filter->alpha_texture);
	gs_effect_set_texture(filter->src_param, filter->src_texture);
	gs_effect_set_float(filter->threshold_param, (float)filter->threshold);
	while (gs_effect_loop(filter->effect, "Draw")) {
		gs_draw_sprite(NULL, 0, filter->width, filter->height);
	}
	obs_source_process_filter_end(filter->context, filter->effect, 0, 0);
}

static void nv_greenscreen_filter_render(void *data, gs_effect_t *effect)
{
	struct nv_greenscreen_data *filter = (struct nv_greenscreen_data *)data;
	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	gs_texrender_t *render;

	if (filter->processing_stop)
		return;

	/* Skip if processing of a frame hasn't yet started */
	if (!filter->target_valid || !target || !parent ||
	    filter->processed_frame) {
		obs_source_skip_video_filter(filter->context);
		return;
	}
	// draw first alpha mask
	if (filter->count >= 1) {
		if (process_texture_gr(filter))
			draw_gr(filter);
	}

	if (!filter->processed_frame) {
		/* 1. Retrieves texrender (& texture) by rendering. */
		render = filter->render;
		if (!render) {
			obs_source_skip_video_filter(filter->context);
			return;
		}
		gs_texrender_reset(render);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		if (gs_texrender_begin(render, filter->width, filter->height)) {
			uint32_t parent_flags =
				obs_source_get_output_flags(target);
			bool custom_draw =
				(parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
			bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
			struct vec4 clear_color;

			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, (float)filter->width, 0.0f,
				 (float)filter->height, -100.0f, 100.0f);

			if (target == parent && !custom_draw && !async)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);

			gs_texrender_end(render);
		}
		gs_blend_state_pop();

		if (filter->count <= 1)
			filter->count += 1;
		if (filter->count == 1) {
			obs_source_skip_video_filter(filter->context);
			return;
		}
		filter->processed_frame = true;
	}
	UNUSED_PARAMETER(effect);
}

bool load_nvvfx(void)
{
	if (!load_nv_vfx_libs(0)) {
		blog(LOG_INFO,
		     "[nvidia RTX Greenscreen]: FX disabled, redistributable not found");
		return false;
	}

#define LOAD_SYM_FROM_LIB(sym, lib, dll)                               \
	if (!(sym = (sym##_t)GetProcAddress(lib, #sym))) {             \
		DWORD err = GetLastError();                            \
		printf("[nvidia RTX Greenscreen]: Couldn't load " #sym \
		       " from " dll ": %lu (0x%lx)",                   \
		       err, err);                                      \
		release_nv_vfx(0);                                     \
		goto unload_everything;                                \
	}

#define LOAD_SYM(sym) \
	LOAD_SYM_FROM_LIB(sym, nv_videofx[0], "NVVideoEffects.dll")
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
#define LOAD_SYM(sym) LOAD_SYM_FROM_LIB(sym, nv_cvimage[0], "NVCVImage.dll")
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
#define LOAD_SYM(sym) LOAD_SYM_FROM_LIB(sym, nv_cudart[0], "cudart64_110.dll")
	LOAD_SYM(cudaMalloc);
	LOAD_SYM(cudaStreamSynchronize);
	LOAD_SYM(cudaFree);
	LOAD_SYM(cudaMemcpy);
	LOAD_SYM(cudaMemsetAsync);
#undef LOAD_SYM

	int err;
	NvVFX_Handle h = NULL;

	/* load the effect to check if the GPU is supported */
	err = NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &h);
	if (err != NVCV_SUCCESS) {
		if (err == NVCV_ERR_UNSUPPORTEDGPU) {
			blog(LOG_INFO,
			     "[nvidia video FX]: disabled, unsupported GPU");
		} else {
			blog(LOG_ERROR, "[nvidia video FX]: disabled, error %i",
			     err);
		}
		goto unload_everything;
	}
	NvVFX_DestroyEffect(h);
	nvvfx_loaded = true;
	blog(LOG_INFO, "[nvidia video FX]: enabled, redistributable found");
	return true;

unload_everything:
	nvvfx_loaded = false;
	release_nv_vfx(0);
	return false;
}

void unload_nvvfx(void)
{
	release_nv_vfx(0);
}

struct obs_source_info nvidia_greenscreen_filter_info = {
	.id = "nv_greenscreen_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = nv_greenscreen_filter_name,
	.create = nv_greenscreen_filter_create,
	.destroy = nv_greenscreen_filter_destroy,
	.get_defaults = nv_greenscreen_filter_defaults,
	.get_properties = nv_greenscreen_filter_properties,
	.update = nv_greenscreen_filter_update,
	.video_render = nv_greenscreen_filter_render,
	.video_tick = nv_greenscreen_filter_tick,
};
