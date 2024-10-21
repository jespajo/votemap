All the clockwise/counter-clockwise stuff is confusing, so I'm writing some documentation, though I'm not sure where it belongs yet.

First, different data do it differently:

The way Postgis represents polygons---in a format called extended well-known binary, or EWKB---follows a standard called OGC Simple Features.
In this standard, the first ring in a polygon is always the outer ring.
The outer ring's points are in counter-clockwise order.
Subsequent rings are holes with their points given clockwise.
In a Postgres query, we can reverse the orientation of the points with `ST_ForcePolygonCW()`.

In ESRI shapefiles, the orientation of points is reversed: clockwise points define an outer ring and counter-clockwise points define a hole.
Also, the order of rings in a polygon doesn't matter; whether each ring defines a positive or negative space is entirely determined by the order of the points.
This means that ESRI shapefiles can define polygons with multiple outer rings---you'd need a MultiPolygon to represent the same shape in EWKB.

When we draw on the screen using the HTML canvas 2D context, we have the origin in the top-left corner, with Y increasing downwards.
But in WebGL, the origin is in the bottom-left.
In the database, the shapes are stored in ESPG:3577, which means units are metres increasing in the north-east direction.
Flipping the Y-axis reverses the orientations of all the rings!!

So it's a bit of a mess.

The first thing we do when we get points from the database on the back end is flip the Y-axis.
This happens in `parse_polygons()` and `parse_path()`.
That means that all of our geometry processing works with the assumption of a Y-axis that increases downwards.
It also means that when we receive map coordinates from the front end, we need to remember to negate their Y values before using them in the database.
