All the clockwise/counter-clockwise stuff is confusing, so I'm writing some documentation, though I'm not sure where it belongs yet.

First, different data do it differently:

Postgis/EWKB follows the OGC Simple Features standard.
In this standard, the first ring in a polygon is always the outer ring.
The outer ring's points are counter-clockwise.
Subsequent rings are holes with points given in clockwise order.
We can reverse the orientation of the points with `ST_ForcePolygonCW()`.

In ESRI shapefiles, polygons are completely different.
The orientation of points is reversed: clockwise points define an outer ring and counter-clockwise points define a hole.
What's more, the order of rings in a polygon doesn't matter---so whether each ring defines a positive or negative space is entirely determined by the order of the ring's points.
This means that ESRI shapefiles can have polygons with multiple outer rings.
You'd need a MultiPolygon to represent the same shape in EWKB.

When we're drawing on the screen, we generally like to have the origin in the top-left corner, with Y increasing downwards.
But we're storing these shapes in ESPG:3577, which means units are metres increasing in the north-east direction.
Flipping the Y-axis reverses the orientations of all the rings!

So here's what we're doing:
Whenever we use the `Polygon` or `Path` types, we ensure the Y-axis increases downwards, flipping the EWKB data by multiplying the Ys by zero.
