#
# Tencent is pleased to support the open source community by making Libco available.
# 
# Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
# 
# Licensed under the Apache License, Version 2.0 (the "License"); 
# you may not use this file except in compliance with the License. 
# You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, 
# software distributed under the License is distributed on an "AS IS" BASIS, 
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
# See the License for the specific language governing permissions and 
# limitations under the License.
#


COMM_MAKE = 1
COMM_ECHO = 1
version=0.5
v=debug
include ../co.mk

########## options ##########
CFLAGS += -I../ -g -fno-strict-aliasing -O2 --std=c++17 -Wall -export-dynamic \
	-Wall -pipe  -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64

UNAME := $(shell uname -s)

ifeq ($(UNAME), FreeBSD)
LINKS += -g -L../lib -lcolib -lpthread
else
LINKS += -g -L../lib -lcolib -lpthread -ldl
endif

PROGS = test_co_routine test_co_async

all:$(PROGS)

test_co_routine:test_co_routine.o
	$(BUILDEXE)

test_co_async:test_co_async.o
	$(BUILDEXE)


dist: clean

clean:
	$(CLEAN) *.o $(PROGS)

