#pragma once

#include <stddef.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct khashp_t;

struct dentry_t
{
};

struct dentry_key_t
{
    struct dentry_t* dentry;
    char* name;
};

class VirtualFileSystem
{
public:
	explicit VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps);
	~VirtualFileSystem();

	uint32_t HashDentryKey(const dentry_key_t* key) const;
	bool PutDentryKey(dentry_key_t* key);
	const dentry_key_t* GetDentryKeyByName(const char* name) const;

private:
	ResourceLayerCaps* ResourceLayerImportCaps;
	khashp_t* DentryKeyByName;
};
