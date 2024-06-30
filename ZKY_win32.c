#define ABS(X) ((X) >= 0 ? (X) : -(X))
#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#define CLAMP(LO, X, HI) MAX((LO), MIN((X), (HI)))
#define ALIGNUP(X, Y) ((X) + ((Y) - 1) & ~((Y) - 1))
#define ALIGNDOWN(X, Y) ((X) & ~((Y) - 1))

#define LENGTH(ARRAY) (sizeof(ARRAY) / sizeof((ARRAY)[0]))
#define OFFSETOF(TYPE, FIELD) ((usize) &((TYPE *) 0)->FIELD)
#define ALIGNOF(TYPE) OFFSETOF(struct { char x; TYPE t; }, t)

#define no_return __declspec(noreturn) void

typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef long long ssize;

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;

typedef float f32;
typedef double f64;

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <winsock2.h>

static struct {
	u16 screen_width;
	u16 screen_height;

	HINSTANCE hinstance;
	HANDLE stdin;
	HANDLE stderr;
	HWND hwnd;
	HDC hdc;
} g;

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#define VK_CHECK(ERR) VK_ASSERT((err = (ERR)) == VK_SUCCESS)
#define VK_ASSERT(X) do if (!(X)) goto vk_error; while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageTypes,
	VkDebugUtilsMessengerCallbackDataEXT const *pCallbackData,
	void *pUserData)
{
	(void) (messageSeverity, messageTypes, pUserData);
	char const *a = pCallbackData->pMessage;
	while (*a) ++a;
	WriteConsoleA(g.stderr, pCallbackData->pMessage, (unsigned long) (a - pCallbackData->pMessage), 0, 0);
	WriteConsoleA(g.stderr, "\r\n", 2, 0, 0);
	return 0;
}

static struct {
	VkInstance instance;
	PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
	VkDebugUtilsMessengerEXT debug_messenger;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical_device;
	VkDevice device;
	u32 graphics_queue_family_index;
	u32 present_queue_family_index;
	VkQueue graphics_queue;
	VkQueue present_queue;
	VkCommandPool graphics_command_pool;
	VkRenderPass main_render_pass;

	u8 current_frame;
#define MAX_SIMULTANEOUS_RENDER_FRAMES 3
	VkCommandBuffer graphics_command_buffers[MAX_SIMULTANEOUS_RENDER_FRAMES];
	VkFence graphics_queue_fences[MAX_SIMULTANEOUS_RENDER_FRAMES];
	VkSemaphore image_acquired_semaphores[MAX_SIMULTANEOUS_RENDER_FRAMES];
	VkSemaphore render_complete_semaphores[MAX_SIMULTANEOUS_RENDER_FRAMES];

	u8 resized;
	VkSurfaceFormatKHR swapchain_format;
	VkExtent2D swapchain_extent;
	VkSwapchainKHR swapchain;
	u32 swapchain_image_count;
#define MAX_SWAPCHAIN_IMAGES 16
	VkImage swapchain_images[MAX_SWAPCHAIN_IMAGES];
	VkImageView swapchain_image_views[MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer swapchain_framebuffers[MAX_SWAPCHAIN_IMAGES];
} vk;

static void vulkan_deinit(void);

static void swapchain_init(void) {
	VkResult err;

	VkSurfaceCapabilitiesKHR surface_capabilities;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical_device, vk.surface, &surface_capabilities));

	VkPresentModeKHR present_modes[16];
	u32 present_modes_count = LENGTH(present_modes);
	err = vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical_device, vk.surface,
		&present_modes_count, present_modes);
	VK_ASSERT(err == VK_SUCCESS || err == VK_INCOMPLETE);

	VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
	for (u32 i = 0; i < present_modes_count; ++i) {
		if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
			swapchain_present_mode = present_modes[i];
			break;
		}
	}

	u32 swapchain_min_image_count = surface_capabilities.minImageCount + 1;
	if (surface_capabilities.maxImageCount != 0) {
		swapchain_min_image_count = MIN(swapchain_min_image_count, surface_capabilities.maxImageCount);
	}

	vk.swapchain_extent = surface_capabilities.currentExtent;
	if (surface_capabilities.currentExtent.width == ~(u32) 0) {
		vk.swapchain_extent.width = g.screen_width;
		vk.swapchain_extent.height = g.screen_height;
	}

	VK_CHECK(vkCreateSwapchainKHR(vk.device, &(VkSwapchainCreateInfoKHR) {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.pNext = &(VkSurfaceFullScreenExclusiveInfoEXT) {
			.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
			.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT,
		},
		.surface = vk.surface,
		.minImageCount = swapchain_min_image_count,
		.imageFormat = vk.swapchain_format.format,
		.imageColorSpace = vk.swapchain_format.colorSpace,
		.imageExtent = vk.swapchain_extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = vk.graphics_queue_family_index == vk.present_queue_family_index ?
			VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
		.queueFamilyIndexCount = vk.graphics_queue_family_index == vk.present_queue_family_index ? 0 : 2,
		.pQueueFamilyIndices = (u32 []) {vk.graphics_queue_family_index, vk.present_queue_family_index},
		.preTransform = surface_capabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = swapchain_present_mode,
		.clipped = VK_TRUE,
		.oldSwapchain = 0,
	}, 0, &vk.swapchain));

	vk.swapchain_image_count = MAX_SWAPCHAIN_IMAGES;
	err = vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swapchain_image_count, vk.swapchain_images);
	VK_ASSERT(err == VK_SUCCESS || err == VK_INCOMPLETE);

	for (u32 i = 0; i < vk.swapchain_image_count; ++i) {
		VK_CHECK(vkCreateImageView(vk.device, &(VkImageViewCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = vk.swapchain_images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = vk.swapchain_format.format,
			.subresourceRange = (VkImageSubresourceRange) {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		}, 0, vk.swapchain_image_views + i));

		VK_CHECK(vkCreateFramebuffer(vk.device, &(VkFramebufferCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = vk.main_render_pass,
			.attachmentCount = 1,
			.pAttachments = (VkImageView []) {vk.swapchain_image_views[i]},
			.width = vk.swapchain_extent.width,
			.height = vk.swapchain_extent.height,
			.layers = 1,
		}, 0, vk.swapchain_framebuffers + i));
	}

	return;
vk_error:
	vulkan_deinit();
}

static void swapchain_deinit(void) {
	vkDeviceWaitIdle(vk.device);
	if (vk.swapchain) {
		for (u32 i = 0; i < vk.swapchain_image_count; ++i) {
			vkDestroyFramebuffer(vk.device, vk.swapchain_framebuffers[i], 0);
			vkDestroyImageView(vk.device, vk.swapchain_image_views[i], 0);
		}
		vkDestroySwapchainKHR(vk.device, vk.swapchain, 0);
	}
}

static void swapchain_reinit(void) {
	if (g.screen_width != 0 && g.screen_height != 0) {
		swapchain_deinit();
		swapchain_init();
	}
}

static void vulkan_init(void) {
	VkResult err;

	VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = 0x1111,
		.messageType = 0x7,
		.pfnUserCallback = vulkan_debug_callback,
	};

	char const *instance_extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
	VK_CHECK(vkCreateInstance(&(VkInstanceCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = &debug_utils_messenger_create_info,
		.pApplicationInfo = &(VkApplicationInfo) {
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.apiVersion = VK_API_VERSION_1_1,
		},
		.enabledExtensionCount = LENGTH(instance_extensions),
		.ppEnabledExtensionNames = instance_extensions,
	}, 0, &vk.instance));

	PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT =
		(PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(vk.instance, "vkCreateDebugUtilsMessengerEXT");
	VK_ASSERT(vkCreateDebugUtilsMessengerEXT);
	vk.vkDestroyDebugUtilsMessengerEXT =
		(PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(vk.instance, "vkDestroyDebugUtilsMessengerEXT");
	VK_ASSERT(vk.vkDestroyDebugUtilsMessengerEXT);

	VK_CHECK(vkCreateDebugUtilsMessengerEXT(vk.instance,
		&debug_utils_messenger_create_info, 0, &vk.debug_messenger));

	VK_CHECK(vkCreateWin32SurfaceKHR(vk.instance, &(VkWin32SurfaceCreateInfoKHR) {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = g.hinstance,
		.hwnd = g.hwnd,
	}, 0, &vk.surface));

	u32 physical_devices_count = 1;
	err = vkEnumeratePhysicalDevices(vk.instance, &physical_devices_count, &vk.physical_device);
	VK_ASSERT(err == VK_SUCCESS || err == VK_INCOMPLETE);

	VkPhysicalDeviceProperties physical_device_properties;
	vkGetPhysicalDeviceProperties(vk.physical_device, &physical_device_properties);

	VkQueueFamilyProperties queue_family_properties[16];
	u32 queue_family_properties_count = LENGTH(queue_family_properties);
	vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device,
		&queue_family_properties_count, queue_family_properties);

	vk.graphics_queue_family_index = ~(u32) 0;
	vk.present_queue_family_index = ~(u32) 0;
	for (u32 i = 0; i < queue_family_properties_count; ++i) {
		if (queue_family_properties[i].queueCount == 0) continue;

		if (vk.graphics_queue_family_index == ~(u32) 0 &&
			(queue_family_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			vk.graphics_queue_family_index = i;
		}

		VkBool32 present_supported;
		VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical_device, i,
			vk.surface, &present_supported));
		if (vk.present_queue_family_index == ~(u32) 0 &&
			present_supported)
		{
			vk.present_queue_family_index = i;
		}

		if (vk.graphics_queue_family_index != ~(u32) 0 &&
			vk.present_queue_family_index != ~(u32) 0)
		{
			break;
		}
	}
	VK_ASSERT(vk.graphics_queue_family_index != ~(u32) 0);
	VK_ASSERT(vk.present_queue_family_index != ~(u32) 0);

	char const *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME};
	VK_CHECK(vkCreateDevice(vk.physical_device, &(VkDeviceCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount =
			vk.graphics_queue_family_index == vk.present_queue_family_index ? 1 : 2,
		.pQueueCreateInfos = (VkDeviceQueueCreateInfo []) {
			{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = vk.graphics_queue_family_index,
				.queueCount = 1,
				.pQueuePriorities = (f32 []) {1.0f},
			},
			{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = vk.present_queue_family_index,
				.queueCount = 1,
				.pQueuePriorities = (f32 []) {1.0f},
			},
		},
		.enabledExtensionCount = LENGTH(device_extensions),
		.ppEnabledExtensionNames = device_extensions,
	}, 0, &vk.device));

	vkGetDeviceQueue(vk.device, vk.graphics_queue_family_index, 0, &vk.graphics_queue);
	vkGetDeviceQueue(vk.device, vk.present_queue_family_index, 0, &vk.present_queue);

	VK_CHECK(vkCreateCommandPool(vk.device, &(VkCommandPoolCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = vk.graphics_queue_family_index,
	}, 0, &vk.graphics_command_pool));


	VkSurfaceFormatKHR surface_formats[16];
	u32 surface_formats_count = LENGTH(surface_formats);
	err = vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical_device, vk.surface,
		&surface_formats_count, surface_formats);
	VK_ASSERT(err == VK_SUCCESS || err == VK_INCOMPLETE);

	vk.swapchain_format = surface_formats[0];
	for (u32 i = 0; i < surface_formats_count; ++i) {
		if (surface_formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
			surface_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			vk.swapchain_format = surface_formats[i];
			break;
		}
	}

	VK_CHECK(vkCreateRenderPass(vk.device, &(VkRenderPassCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = (VkAttachmentDescription []) {
			{
				.format = vk.swapchain_format.format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			},
		},
		.subpassCount = 1,
		.pSubpasses = (VkSubpassDescription []) {
			{
				.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.colorAttachmentCount = 1,
				.pColorAttachments = (VkAttachmentReference []) {
					{
						.attachment = 0,
						.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					},
				},
			},
		},
	}, 0, &vk.main_render_pass));

	VK_CHECK(vkAllocateCommandBuffers(vk.device, &(VkCommandBufferAllocateInfo) {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = vk.graphics_command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = MAX_SIMULTANEOUS_RENDER_FRAMES,
	}, vk.graphics_command_buffers));

	for (u8 i = 0; i < MAX_SIMULTANEOUS_RENDER_FRAMES; ++i) {
		VK_CHECK(vkCreateFence(vk.device, &(VkFenceCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT,
		}, 0, vk.graphics_queue_fences + i));

		VK_CHECK(vkCreateSemaphore(vk.device, &(VkSemaphoreCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		}, 0, vk.image_acquired_semaphores + i));

		VK_CHECK(vkCreateSemaphore(vk.device, &(VkSemaphoreCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		}, 0, vk.render_complete_semaphores + i));
	}

	swapchain_init();

	return;
vk_error:
	vulkan_deinit();
}

static void vulkan_deinit(void) {
	if (vk.instance) {
		if (vk.device) {
			swapchain_deinit();
			for (u8 i = 0; i < MAX_SIMULTANEOUS_RENDER_FRAMES; ++i) {
				if (vk.render_complete_semaphores[i]) vkDestroySemaphore(vk.device, vk.render_complete_semaphores[i], 0);
				if (vk.image_acquired_semaphores[i]) vkDestroySemaphore(vk.device, vk.image_acquired_semaphores[i], 0);
				if (vk.graphics_queue_fences[i]) vkDestroyFence(vk.device, vk.graphics_queue_fences[i], 0);
			}
			if (vk.graphics_command_pool) vkDestroyCommandPool(vk.device, vk.graphics_command_pool, 0);
			if (vk.main_render_pass) vkDestroyRenderPass(vk.device, vk.main_render_pass, 0);
			vkDestroyDevice(vk.device, 0);
		}
		if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, 0);
		if (vk.debug_messenger) vk.vkDestroyDebugUtilsMessengerEXT(vk.instance, vk.debug_messenger, 0);
		vkDestroyInstance(vk.instance, 0);
	}

	memset(&vk, 0, sizeof(vk));

	u8 c; ReadConsoleA(g.stdin, &c, 1, 0, 0);
}

static void vulkan_resize(void) {
	vk.resized = 1;
}

static void vulkan_present(void) {
	VkResult err;

	VK_CHECK(vkWaitForFences(vk.device, 1, (VkFence []) {vk.graphics_queue_fences[vk.current_frame]}, 1, ~(u64) 0));

	u32 image_index;
	err = vkAcquireNextImageKHR(vk.device, vk.swapchain, ~(u64) 0, vk.image_acquired_semaphores[vk.current_frame], 0, &image_index);
	if (err == VK_ERROR_OUT_OF_DATE_KHR) {
		swapchain_reinit();
		return;
	}
	VK_ASSERT(err == VK_SUCCESS || err == VK_SUBOPTIMAL_KHR);

	VK_CHECK(vkResetFences(vk.device, 1, (VkFence []) {vk.graphics_queue_fences[vk.current_frame]}));
	VK_CHECK(vkBeginCommandBuffer(vk.graphics_command_buffers[vk.current_frame], &(VkCommandBufferBeginInfo) {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	}));
		vkCmdBeginRenderPass(vk.graphics_command_buffers[vk.current_frame], &(VkRenderPassBeginInfo) {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = vk.main_render_pass,
			.framebuffer = vk.swapchain_framebuffers[image_index],
			.renderArea = (VkRect2D) {
				.offset = {0, 0},
				.extent = vk.swapchain_extent,
			},
			.clearValueCount = 1,
			.pClearValues = (VkClearValue []) {
				{.color.float32 = {0.2f, 0.2f, 0.2f, 1.0f}},
			},
		}, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(vk.graphics_command_buffers[vk.current_frame]);
	VK_CHECK(vkEndCommandBuffer(vk.graphics_command_buffers[vk.current_frame]));
	VK_CHECK(vkQueueSubmit(vk.graphics_queue, 1, &(VkSubmitInfo) {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = (VkSemaphore []) {vk.image_acquired_semaphores[vk.current_frame]},
		.pWaitDstStageMask = (VkPipelineStageFlags []) {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT},
		.commandBufferCount = 1,
		.pCommandBuffers = (VkCommandBuffer []) {vk.graphics_command_buffers[vk.current_frame]},
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = (VkSemaphore []) {vk.render_complete_semaphores[vk.current_frame]},
	}, vk.graphics_queue_fences[vk.current_frame]));

	err = vkQueuePresentKHR(vk.present_queue, &(VkPresentInfoKHR) {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = (VkSemaphore []) {vk.render_complete_semaphores[vk.current_frame]},
		.swapchainCount = 1,
		.pSwapchains = (VkSwapchainKHR []) {vk.swapchain},
		.pImageIndices = (u32 []) {image_index},
	});
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR || vk.resized) {
		vk.resized = 0;
		swapchain_reinit();
	} else VK_ASSERT(err == VK_SUCCESS);

	vk.current_frame = (vk.current_frame + 1) % MAX_SIMULTANEOUS_RENDER_FRAMES;

	return;
vk_error:
	;
}

static ssize WINAPI window_proc(HWND hwnd, unsigned int message, usize wParam, ssize lParam) {
	ssize result = 0;
	switch (message) {
		case WM_PAINT: {
			ValidateRect(hwnd, 0);
		} break;
		case WM_ERASEBKGND: {
			result = 1;
		} break;
		case WM_SIZE: {
			g.screen_width = (u16) (usize) lParam;
			g.screen_height = (u16) ((usize) lParam >> 16);

			vulkan_resize();
		} break;
		case WM_CREATE: {
			g.hwnd = hwnd;
			g.hdc = GetDC(hwnd);

			vulkan_init();
		} break;
		case WM_DESTROY: {
			vulkan_deinit();

			PostQuitMessage(0);
		} break;
		default: {
			if (!(message == WM_SYSCOMMAND && wParam == SC_KEYMENU)) {
				result = DefWindowProcW(hwnd, message, wParam, lParam);
			}
		} break;
	}
	return result;
}

no_return WINAPI WinMainCRTStartup(void) {
	g.hinstance = GetModuleHandleW(0);

	AllocConsole();
	g.stdin = GetStdHandle(STD_INPUT_HANDLE);
	g.stderr = GetStdHandle(STD_ERROR_HANDLE);

	WSADATA wsadata;
	u8 networking_supported = WSAStartup(0x202, &wsadata) == 0;

	u8 sleep_is_granular = timeBeginPeriod(1) == 0;

	SetProcessDPIAware();
	WNDCLASSEXW wndclass = {0};
	wndclass.cbSize = sizeof(WNDCLASSEXW);
	wndclass.style = CS_OWNDC;
	wndclass.lpfnWndProc = window_proc;
	wndclass.hInstance = g.hinstance;
	wndclass.hIcon = LoadIconW(0, IDI_WARNING);
	wndclass.hCursor = LoadCursorW(0, IDC_CROSS);
	wndclass.lpszClassName = L"A";
	RegisterClassExW(&wndclass);
	CreateWindowExW(0, wndclass.lpszClassName, L"ZKY",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		0, 0, g.hinstance, 0);

	for (;;) {
		MSG msg;
		while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			switch (msg.message) {
				case WM_KEYDOWN:
				case WM_KEYUP:
				case WM_SYSKEYDOWN:
				case WM_SYSKEYUP: {
					u8 pressed = ((usize) msg.lParam & (usize) 1 << 31) == 0;
					u8 repeat = pressed && ((usize) msg.lParam & (usize) 1 << 30) != 0;
					u8 sys = msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP;
					u8 alt = sys && ((usize) msg.lParam & (usize) 1 << 29) != 0;

					if (!repeat && (!sys || alt || msg.wParam == VK_F10)) {
						if (msg.wParam == VK_F4 && alt) DestroyWindow(g.hwnd);
						if (msg.wParam == VK_ESCAPE) DestroyWindow(g.hwnd);
					}
				} break;
				case WM_QUIT: {
					goto main_loop_end;
				} break;
				default: {
					DispatchMessageW(&msg);
				} break;
			}
		}

		vulkan_present();

		if (sleep_is_granular) {
			Sleep(1);
		}
	}
main_loop_end:

	if (networking_supported) WSACleanup();
	ExitProcess(0);
}
