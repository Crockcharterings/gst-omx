/*
 * Copyright (C) 2013 Simon Busch <morphis@gravedo.de>
 *               2012 Canonical Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HYBRIS_BINDING_H_
#define HYBRIS_BINDING_H_

void *android_dlopen(const char *filename, int flag);
void *android_dlsym(void *name, const char *symbol);
void android_dlclose(void *handle);

#endif
