#include "PincelRasterizer.hpp"

namespace Volcano::GUI {

PincelRasterizer::PincelRasterizer(VkRenderPass renderPass)
	: renderPass(renderPass)
{
	//
}

PincelRasterizer::~PincelRasterizer()
{
	//
}

void PincelRasterizer::Upload(const Pincel::Texture& texture)
{
	//
}

void PincelRasterizer::Record(VkCommandBuffer commandBuffer)
{
	//
}

void CreateImage(uint32_t width, uint32_t height)
{
	//
}

void DestroyImage()
{
	//
}

} // namespace Volcano::GUI
