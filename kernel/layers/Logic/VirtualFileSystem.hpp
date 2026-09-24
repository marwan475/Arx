#pragma once

struct ResourceLayerCaps;

class VirtualFileSystem
{
public:
	explicit VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps);
	~VirtualFileSystem() = default;

private:
	ResourceLayerCaps* ResourceLayerImportCaps;
};
