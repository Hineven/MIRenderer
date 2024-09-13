/*
 * Created: 2024/7/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_DEVICE_SHARED_H
#define MIRENDERER_RHI_DEVICE_SHARED_H

// Specify the bindless uniform buffer name used within shaders.
// Used for storing the bindless resource indices.
// Also the RHI layer relies on its presence to configure pipeline layout.
#define BINDLESS_TABLE_UNIFORM_BUFFER_NAME __internal__BindlessIndicesBuffer

#endif //MIRENDERER_RHI_DEVICE_SHARED_H
