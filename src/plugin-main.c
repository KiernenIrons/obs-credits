#include <obs-module.h>
#include "credits-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-credits", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&credits_source_info);
	blog(LOG_INFO, "[obs-credits] Plugin loaded (version %s)",
	     OBS_CREDITS_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-credits] Plugin unloaded");
}
