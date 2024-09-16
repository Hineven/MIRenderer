/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_PLATFORM_H
#define MIRENDERER_CORE_PLATFORM_H

#ifdef MI_COMPILER_MSVC
#define FORCEINLINE __forceinline
#endif

#ifdef MI_COMPILER_GNU
#ifndef NDEBUG
	#define FORCEINLINE inline 											/* Don't force code to be inline, or you'll run into -Wignored-attributes */
#else
	#define FORCEINLINE inline __attribute__ ((always_inline))			/* Force code to be inline */
#endif
#endif

#ifndef FORCEINLINE
#define FORCEINLINE inline
#warning "FORCEINLINE is not defined for this compiler"
#endif

#endif //MIRENDERER_CORE_PLATFORM_H
