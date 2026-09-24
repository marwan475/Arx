#include "layers/Logic/VirtualFileSystem.hpp"

extern "C"
{
#include <klib/khashl/khashp.h>
}

#include <klib/klib.h>

VirtualFileSystem::VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps)
{
	ResourceLayerImportCaps = resourceLayerCaps;
	DentryKeyByName         = khp_str_init(sizeof(dentry_key_t*), 0);
}

VirtualFileSystem::~VirtualFileSystem()
{
	if (DentryKeyByName != nullptr)
	{
		khp_str_destroy(DentryKeyByName);
		DentryKeyByName = nullptr;
	}
}

uint32_t VirtualFileSystem::HashDentryKey(const dentry_key_t* key) const
{
	if (key == nullptr)
	{
		return 0;
	}

	uint32_t       hash       = 2166136261U; // FNV-1a
	const uint8_t* dentryData = (const uint8_t*) &key->dentry;

	for (size_t i = 0; i < sizeof(key->dentry); ++i)
	{
		hash ^= dentryData[i];
		hash *= 16777619U;
	}

	if (key->name != nullptr)
	{
		for (const uint8_t* p = (const uint8_t*) key->name; *p != 0; ++p)
		{
			hash ^= *p;
			hash *= 16777619U;
		}
	}

	return hash;
}

bool VirtualFileSystem::PutDentryKey(dentry_key_t* key)
{
	if (DentryKeyByName == nullptr || key == nullptr || key->name == nullptr)
	{
		return false;
	}

	int     absent = 0;
	khint_t k      = khp_str_put(DentryKeyByName, key->name, &absent);
	if (k == khp_end(DentryKeyByName))
	{
		return false;
	}

	khp_set_val(DentryKeyByName, k, &key);
	return true;
}

const dentry_key_t* VirtualFileSystem::GetDentryKeyByName(const char* name) const
{
	if (DentryKeyByName == nullptr || name == nullptr)
	{
		return nullptr;
	}

	khint_t k = khp_str_get(DentryKeyByName, name);
	if (k == khp_end(DentryKeyByName))
	{
		return nullptr;
	}

	dentry_key_t* value = nullptr;
	khp_get_val(DentryKeyByName, k, &value);
	if (value == nullptr)
	{
		return nullptr;
	}

	return value;
}
