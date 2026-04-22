// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <Varjo_types_datastream.h>

class IApplication
{
public:
    struct Options {
        varjo_ChannelFlag channels;
    };

    virtual ~IApplication() = default;

    virtual void run() = 0;
    virtual void terminate() = 0;
};
