# Copyright (C) 2018 Istituto Italiano di Tecnologia (IIT). All rights reserved.
# This software may be modified and distributed under the terms of the
# GNU Lesser General Public License v2.1 or any later version.

add_subdirectory(IWear)
add_subdirectory(IWearActuators)
add_subdirectory(IXsensMVNControl)

if(ENABLE_Logger)
    add_subdirectory(IWearLogger)
endif()


