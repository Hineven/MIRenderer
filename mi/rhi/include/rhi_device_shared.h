/*
 * Created: 2024/7/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_DEVICE_SHARED_H
#define MIRENDERER_RHI_DEVICE_SHARED_H

#define BINDLESS_RESOURCE_ARRAY_PREFIX __internal__BindlessIndicesBuffer_

// Used to access bindless resources in shaders
// Available types: Texture, Buffer, AccelerationStructure
#define BINDLESS_RESOURCE(Type, Index) (BINDLESS_RESOURCE_ARRAY_PREFIX ##Type[Index])

#endif //MIRENDERER_RHI_DEVICE_SHARED_H
