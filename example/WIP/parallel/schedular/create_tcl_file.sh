#!/bin/bash
#
# Copyright 2003-2012 Jeffrey K. Hollingsworth
#
# This file is part of Active Harmony.
#
# Active Harmony is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Active Harmony is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Active Harmony.  If not, see <http://www.gnu.org/licenses/>.
#

## This is where you define the parameters
## For this example, it is only one parameter.


echo "  harmonyApp APPTest {
    { harmonyBundle param1 {
       int {0 19 1} global
    }
    }
    { obsGoodness -1000 1000 global }
    { predGoodness -300 -100 }
    } $1"
