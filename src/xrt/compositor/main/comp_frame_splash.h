// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Startup splash: the frame shell logo, head-locked on black, until the head is first tracked.
 * @ingroup comp_main
 */

#pragma once

struct comp_compositor;
struct sf_comp_splash;

struct sf_comp_splash *
sf_comp_splash_create(struct comp_compositor *c);

// Until the head is first tracked, replaces this frame's layers with the logo
void
sf_comp_splash_apply(struct sf_comp_splash *s);

void
sf_comp_splash_destroy(struct sf_comp_splash **s);
