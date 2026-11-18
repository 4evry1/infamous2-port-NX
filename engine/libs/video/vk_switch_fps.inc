/* One display-resolution triangle in the upper left. No guest descriptors or staging allocations. */
#include <hz_build.h>
#include <hz_fps_hud.h>

enum { HUD_CELL_WIDTH = 12, HUD_CELL_HEIGHT = 18, HUD_COLUMNS = 36 };
static const char hud_alphabet[] = " 0123456789.hz-BASEOTLFP";
static const VkRect2D hud_rect = {{0,0}, {HUD_COLUMNS * HUD_CELL_WIDTH, HUD_CELL_HEIGHT}};
static const char hud_fragment[] =
    "#version 450\n"
    "layout(push_constant) uniform Text { uint letters[9]; } text;"
    "layout(location=0) out vec4 color;"
    // Five bits per row, top to bottom. The first word holds four rows, the second three.
    "const uvec2 font[] = uvec2[]("
    "uvec2(0x00000u,0x0000u)," /*   */
    "uvec2(0xace2eu,0x3a39u)," /* 0 */
    "uvec2(0x21184u,0x3884u)," /* 1 */
    "uvec2(0x1062eu,0x7d04u)," /* 2 */
    "uvec2(0x7043eu,0x7821u)," /* 3 */
    "uvec2(0x928c2u,0x085fu)," /* 4 */
    "uvec2(0xf421fu,0x7821u)," /* 5 */
    "uvec2(0xf420eu,0x3a31u)," /* 6 */
    "uvec2(0x2083fu,0x2108u)," /* 7 */
    "uvec2(0x7462eu,0x3a31u)," /* 8 */
    "uvec2(0x7c62eu,0x3821u)," /* 9 */
    "uvec2(0x00000u,0x1000u)," /* . */
    "uvec2(0xcda10u,0x4631u)," /* h */
    "uvec2(0x17c00u,0x7d04u)," /* z */
    "uvec2(0x70000u,0x0000u)," /* - */
    "uvec2(0xf463eu,0x7a31u)," /* B */
    "uvec2(0xfc62eu,0x4631u)," /* A */
    "uvec2(0x7420fu,0x7821u)," /* S */
    "uvec2(0xf421fu,0x7e10u)," /* E */
    "uvec2(0x8c62eu,0x3a31u)," /* O */
    "uvec2(0x2109fu,0x1084u)," /* T */
    "uvec2(0x84210u,0x7e10u)," /* L */
    "uvec2(0xf421fu,0x4210u)," /* F */
    "uvec2(0xf463eu,0x4210u));" /* P */
    "void main() {"
    "  ivec2 p = ivec2(gl_FragCoord.xy); uint cell = uint(p.x / 12);"
    "  uint glyph = (text.letters[cell / 4u] >> ((cell % 4u) * 8u)) & 255u;"
    "  int x = (p.x % 12) / 2; int y = (p.y - 2) / 2;"
    "  bool ink = false;"
    "  if (x < 5 && p.y >= 2 && y < 7) {"
    "    uint bits = font[glyph][y / 4];"
    "    ink = ((bits >> uint((y % 4) * 5 + 4 - x)) & 1u) != 0u;"
    "  }"
    "  color = vec4(vec3(ink ? 0.96 : 0.12), 1.0);"
    "}";

static VkShaderModule vk_pass_module(const char*, int, const char*);
static VkResult vk_fullscreen_pipeline(VkShaderModule, VkShaderModule, VkPipelineLayout,
                                       VkRenderPass, u32, VkPipeline*);
static VkFramebuffer vk_pass_framebuffer(VkRenderPass, const VkImageView*, u32, u32, u32);
static struct {
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkImageView views[8];
    VkFramebuffer frames[8];
    hz_fps_window rate;
    uint32_t packed[HUD_COLUMNS / 4];
} fps_hud;

static void vk_fps_hud_drop(void)
{
    for (size_t i = 0; i < 8; ++i) {
        if (fps_hud.frames[i]) pvkDestroyFramebuffer(s_vk.device, fps_hud.frames[i], NULL);
        if (fps_hud.views[i]) pvkDestroyImageView(s_vk.device, fps_hud.views[i], NULL);
    }
    if (fps_hud.pipeline) pvkDestroyPipeline(s_vk.device, fps_hud.pipeline, NULL);
    if (fps_hud.layout) pvkDestroyPipelineLayout(s_vk.device, fps_hud.layout, NULL);
    if (fps_hud.pass) pvkDestroyRenderPass(s_vk.device, fps_hud.pass, NULL);
    memset(&fps_hud, 0, sizeof fps_hud);
}

static VkExtent2D vk_fps_hud_init(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage)
{
    const char *setting = getenv("HZ_FPS_HUD");
    if ((setting && !strcmp(setting, "0")) || !(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        extent.width < hud_rect.extent.width || extent.height < hud_rect.extent.height)
        return (VkExtent2D){0};
    VkAttachmentDescription attachment = {.format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference slot = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &slot};
    VkSubpassDependency deps[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 0},
    };
    VkRenderPassCreateInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
        .dependencyCount = 2, .pDependencies = deps};
    if (pvkCreateRenderPass(s_vk.device, &rp, NULL, &fps_hud.pass) != VK_SUCCESS) goto failed;
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof fps_hud.packed};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (pvkCreatePipelineLayout(s_vk.device, &layout, NULL, &fps_hud.layout) != VK_SUCCESS) goto failed;
    VkShaderModule vs = vk_pass_module(kVkPassVS, RSX_SHADER_STAGE_VERTEX, "FPS HUD vertex");
    VkShaderModule fs = vk_pass_module(hud_fragment, RSX_SHADER_STAGE_FRAGMENT, "FPS HUD fragment");
    VkResult result = vs && fs ? vk_fullscreen_pipeline(vs, fs, fps_hud.layout, fps_hud.pass, 1, &fps_hud.pipeline)
                              : VK_ERROR_INITIALIZATION_FAILED;
    if (vs) pvkDestroyShaderModule(s_vk.device, vs, NULL);
    if (fs) pvkDestroyShaderModule(s_vk.device, fs, NULL);
    if (result != VK_SUCCESS) goto failed;
    for (u32 i = 0; i < s_vk.sc_count; ++i) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = s_vk.sc_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (pvkCreateImageView(s_vk.device, &view, NULL, &fps_hud.views[i]) != VK_SUCCESS) goto failed;
        fps_hud.frames[i] = vk_pass_framebuffer(fps_hud.pass, &fps_hud.views[i], 1, extent.width, extent.height);
        if (!fps_hud.frames[i]) goto failed;
    }
    VK_LOG("FPS HUD active (%ux%u)\n", hud_rect.extent.width, hud_rect.extent.height);
    return hud_rect.extent;
failed:
    VK_LOG("FPS HUD unavailable for this swapchain\n");
    vk_fps_hud_drop();
    return (VkExtent2D){0};
}

static int vk_fps_hud_record(VkCommandBuffer cmd, u32 index)
{
    if (!fps_hud.pipeline) return 0;
    hz_fps_window *w = &fps_hud.rate;
    if (hz_fps_update(w, ps3_ft_now(), hz_guest_frame_count(), hz_output_frame_count())) {
        char text[HUD_COLUMNS + 1];
        if (w->valid)
            snprintf(text, sizeof text, " %s BASE %u.%u TOTAL %u.%u FPS ", HZ_VERSION,
                     w->base_tenths / 10, w->base_tenths % 10, w->output_tenths / 10, w->output_tenths % 10);
        else
            snprintf(text, sizeof text, " %s BASE -- TOTAL -- FPS ", HZ_VERSION);
        static unsigned announced;
        if (announced < 3) { VK_LOG("FPS HUD%s\n", text); ++announced; }
        memset(fps_hud.packed, 0, sizeof fps_hud.packed);
        for (unsigned i = 0; text[i]; ++i) {
            const char *glyph = strchr(hud_alphabet, text[i]);
            if (glyph) fps_hud.packed[i / 4] |= (uint32_t)(glyph - hud_alphabet) << (8 * (i % 4));
        }
    }
    VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fps_hud.pass, .framebuffer = fps_hud.frames[index], .renderArea = hud_rect};
    VkViewport vp = {0, 0, hud_rect.extent.width, hud_rect.extent.height, 0, 1};
    pvkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fps_hud.pipeline);
    pvkCmdSetViewport(cmd, 0, 1, &vp);
    pvkCmdSetScissor(cmd, 0, 1, &hud_rect);
    pvkCmdPushConstants(cmd, fps_hud.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof fps_hud.packed, fps_hud.packed);
    pvkCmdDraw(cmd, 3, 1, 0, 0);
    pvkCmdEndRenderPass(cmd);
    return 1;
}
