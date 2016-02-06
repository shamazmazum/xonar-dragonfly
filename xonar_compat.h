#ifndef XONAR_COMPAT_H
#define XONAR_COMPAT_H

#if !defined(__DragonFly__) && !defined(__FreeBSD__)
#error "Platform not supported"
#endif

/* malloc / printf stuff */
#if defined __DragonFly__
#define kern_printf(fmt, ...) kprintf (fmt, ##__VA_ARGS__)
#define kern_snprintf(str, size, fmt, ...) ksnprintf (str, size, fmt, ##__VA_ARGS__)
#define kern_malloc(size, type, flags) kmalloc (size, type, flags)
#define kern_free(addr, type) kfree (addr, type)
#elif defined __FreeBSD__
#define kern_printf(fmt, ...) printf (fmt, ##__VA_ARGS__)
#define kern_snprintf(str, size, fmt, ...) snprintf (str, size, fmt, ##__VA_ARGS__)
#define kern_malloc(size, type, flags) malloc (size, type, flags)
#define kern_free(addr, type) free (addr, type)
#endif

/* dma tag creation also differs */
#if defined __DragonFly__
#define xonar_create_dma_tag(tag, maxsize, parent_tag, lock) bus_dma_tag_create (\
	/* parent */ parent_tag,											\
	/* alignment */ 4, /* boundary */ 0,								\
	/* lowaddr */ BUS_SPACE_MAXADDR_32BIT,								\
	/* highaddr */ BUS_SPACE_MAXADDR,									\
	/* filter */ NULL, /* filterarg */ NULL,							\
	/* maxsize */ maxsize, /* nsegments */ 1,							\
	/* maxsegz */ 0x3ffff,												\
	/* flags */ 0, /* result */ tag)
#elif defined __FreeBSD__
#define xonar_create_dma_tag(tag, maxsize, parent_tag, lock) bus_dma_tag_create (\
	/* parent */ parent_tag,										\
	/* alignment */ 4, /* boundary */ 0,							\
	/* lowaddr */ BUS_SPACE_MAXADDR_32BIT,							\
	/* highaddr */ BUS_SPACE_MAXADDR,								\
	/* filter */ NULL, /* filterarg */ NULL,						\
	/* maxsize */ maxsize, /* nsegments */ 1,						\
	/* maxsegz */ 0x3ffff,											\
	/* flags */ 0, /* lock fn */ busdma_lock_mutex,					\
	/* lock */ lock, /* result */ tag)
#endif
#endif
