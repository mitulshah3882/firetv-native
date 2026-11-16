/*
 * Prism Spire: a glass piece's outline, shared by the physics (game.c) and the renderer (render.c).
 * A piece is one or two convex polygons in its local frame (metres, counter-clockwise), extruded to
 * the same depth as every other piece.
 *
 * SPDX-FileCopyrightText: 2026 Mitul Shah
 * SPDX-License-Identifier: MIT
 */
#ifndef PS_SHAPE_H
#define PS_SHAPE_H

#define SHAPE_MAX_PARTS 2
#define SHAPE_MAX_VERTS 8
#define PIECE_DEPTH 1.0f /* every piece is extruded 0.5 m in front of and behind the tower plane */

typedef struct {
    int nparts;
    int nverts[SHAPE_MAX_PARTS];
    float v[SHAPE_MAX_PARTS][SHAPE_MAX_VERTS][2];
} Shape;

#endif
