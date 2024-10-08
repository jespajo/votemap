# Creating Voronoi maps

Old documentation.
We're not making Voronoi maps any more.
But this is how we did it.


## Add another geometry column to the districts table: a MultiPolygon Voronoi diagram of the polling places in that district.

    select addgeometrycolumn('public', 'electorates_22', 'booths_voronoi', 3577, 'MULTIPOLYGON', 2);

    update electorates_22 e
    set booths_voronoi = multipolygon
    from (
        select electorate_name,
          /* We have to do this weird twice-grouping to turn it from a GeometryCollection to a MultiPolygon */
          st_collect(polygon) as multipolygon
        from (
            select electorate_name,
              (st_dump(st_voronoipolygons(st_collect(booth_point), 3.0, envelope))).geom as polygon
            from (
                select e.name as electorate_name,
                  b.geom as booth_point,
                  st_envelope(e.new_geom) as envelope
                from electorates_22 e
                join booths_22 b on e.name = b.electorate_name
                where st_contains(e.new_geom, b.geom)
              ) t
            group by electorate_name, envelope
          ) t
        group by electorate_name
      ) t
    where e.name = t.electorate_name;

    create index electorates_22_booths_voronoi_idx on electorates_22 using gist(booths_voronoi);

We restrict our set of booths to ones where the XML and spatial data agree---that is, the coordinates for the booth fall into the boundaries of the booth's district.

Note that the `booth_voronoi` multipolygons are huge overlapping rectangles---not yet clipped by each district's boundaries.


## Add a geometry column to the booths table to associate booths with polygons from the Voronoi diagrams.

Only a subset of booths will have a value in their `voronoi` field.
If present, the value is the polygon from the overall Voronoi map associated with that booth.
This polygon has been clipped by the boundaries of the booth's district.

Currently we ignore a few cases in our data:
- Booths whose position is, according to our spatial data, not in the correct electoral district.
- Multiple booths with the same location. We just take the first one. This is especially common with pre-polling booths always being a separate booth with the same location.

    select addgeometrycolumn('public', 'booths_22', 'voronoi', 3577, 'MULTIPOLYGON', 2);

    update booths_22 b
    set voronoi = multipolygon
    from (
        with q as (
            select e.name,
              (st_dump(e.booths_voronoi)).geom as v_polygon,
              e.new_geom as e_geom,
              b.geom as b_point,
              b.booth_id
            from electorates_22 e
            join booths_22 b on e.name = b.electorate_name
            where st_contains(e.new_geom, b.geom)
          )
        select (array_agg(booth_id))[1] as booth_id,
          multipolygon
        from (
            select booth_id,
              st_collect(geom) as multipolygon
            from (
                select booth_id,
                  (st_dump(st_intersection(v_polygon, e_geom, 3.0))).geom as geom
                from q
                where st_contains(v_polygon, b_point)
              ) t
            where st_geometrytype(geom) = 'ST_Polygon'
            group by booth_id
          ) t
        group by multipolygon
      ) t
    where b.booth_id = t.booth_id;

    create index booths_22_voronoi_idx on booths_22 using gist(voronoi);


## Create a topogeometry from the Voronoi diagram.

    select createtopology('booths_22_topo', 3577);

    select st_createtopogeo('booths_22_topo', st_collect(voronoi))
    from booths_22;

    select addtopogeometrycolumn('booths_22_topo', 'public', 'booths_22', 'topo', 'MULTIPOLYGON');
    --> Make a note of the layer ID returned.

    update booths_22 b
    set topo = createtopogeom('booths_22_topo', 3, /*LAYER ID:*/1, faces)
    from (
        with b as (
          select b.booth_id,
            f.face_id,
            st_area(st_intersection(b.voronoi, st_getfacegeometry('booths_22_topo', f.face_id))) as area
          from booths_22 b
            inner join booths_22_topo.face f on b.voronoi && f.mbr
        )
        select topoelementarray_agg(ARRAY[b1.face_id, 3]) as faces,
          b1.booth_id
        from b b1
        where area = (select max(area) from b b2 where b1.face_id = b2.face_id)
        group by b1.booth_id
      ) t
    where b.booth_id = t.booth_id;


## Create a geometry from the topogeometry.

This is just so we can make use of a spatial index.

    select addgeometrycolumn('public', 'booths_22', 'new_voronoi', 3577, 'MULTIPOLYGON', 2);

    update booths_22
    set new_voronoi = multipolygon
    from (
        select booth_id, topo as multipolygon from booths_22
      ) t
    where booths_22.booth_id = t.booth_id;

    create index booths_22_new_voronoi_idx on booths_22 using gist(new_voronoi);
