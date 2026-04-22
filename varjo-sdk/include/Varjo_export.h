// Copyright 2019-2020 Varjo Technologies Oy. All rights reserved.

#ifndef VARJO_EXPORT_H
#define VARJO_EXPORT_H

#if defined __cplusplus
extern "C" {
#endif

#ifndef __has_declspec_attribute
#define __has_declspec_attribute(x) 0
#endif

#ifndef VARJORUNTIME_STATIC
#ifdef VARJORUNTIME_EXPORTS
#if defined(_MSC_VER) || __has_declspec_attribute(dllexport)
#define VARJORUNTIME_EXPORT __declspec(dllexport)
#else
#define VARJORUNTIME_EXPORT __attribute__((dllexport))
#endif
#else
#if defined(_MSC_VER) || __has_declspec_attribute(dllimport)
#define VARJORUNTIME_EXPORT __declspec(dllimport)
#else
#define VARJORUNTIME_EXPORT __attribute__((dllimport))
#endif
#endif
#else
#define VARJORUNTIME_EXPORT
#endif

#ifndef VARJORUNTIME_NO_EXPORT
#define VARJORUNTIME_NO_EXPORT
#endif

#ifndef VARJORUNTIME_DEPRECATED
#if defined(_MSC_VER) || __has_declspec_attribute(deprecated)
#define VARJORUNTIME_DEPRECATED __declspec(deprecated)
#else
#define VARJORUNTIME_DEPRECATED __attribute__((deprecated))
#endif
#endif

#define VARJO_API VARJORUNTIME_EXPORT
#define VARJO_EXPERIMENTAL_API VARJORUNTIME_EXPORT
#define VARJO_DEPRECATED_API VARJORUNTIME_EXPORT VARJORUNTIME_DEPRECATED

#if defined __cplusplus
}
#endif

#endif  // VARJO_EXPORT_H
