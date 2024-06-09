Half-finished documentation.

Process:

## 1. Import shapefiles with coastline and district data.

All shapefiles are at e.g. /tmp/shapes/boundaries.shp.
Their coordinate reference system is ESPG:4283 (GDA94 using lon/lat)---project them to ESPG:3577 (Australian Albers using metres).
Put this in a database table called `shp.bounds22` (you have to create the schema `shp` first).
```
    shp2pgsql -D -I -s 4283:3577 /tmp/shapes/boundaries shp.bounds_22 | pq
```
Do the same for the coastline.

## 2. Create a new table for extra info related to districts.

```
    create table electorates_22 (name varchar(32) primary key);

    insert into electorates_22
    select elect_div as name from shp.bounds_22;
```

Create a new geometry column in the district table, clipping each district by the coast:

```
    select addgeometrycolumn('public', 'electorates_22', 'geom', 3577, 'MULTIPOLYGON', 2);

    update electorates_22 as e
    set geom = st_force2d(
        st_multi(st_collectionextract(clipped, 3))
      )
    from (
        select bounds.elect_div,
          st_intersection(bounds.geom, land.geom, 10.0) as clipped
        from shp.bounds_22 bounds
          join (
            select st_union(geom) as geom
            from shp.coast
            where feat_code in ('mainland', 'island')
          ) land on st_intersects(bounds.geom, land.geom)
      ) t
    where e.name = t.elect_div;
```

Create an index for the new column:

```
    create index electorates_22_geom_idx on electorates_22 using gist(geom);
```

## 3. Create a topology for the new geometry column.

```
    select createtopology('electorates_22_topo', 3577);

    select st_createtopogeo('electorates_22_topo', st_collect(geom))
    from electorates_22;
```

Create a topology layer to associate the faces in the newly-created topology with the electorates they represent:

```
    select addtopogeometrycolumn('electorates_22_topo', 'public', 'electorates_22', 'topo', 'MULTIPOLYGON');
```

Make a note of the layer ID returned by the above command and use it in the below command.
The below command creates a new topology column in the electorates table.
This topology column holds the topology's face IDs for each electorate.

```
    -- @Todo: Use findlayer() to get the layer ID dynamically:
    --set topo = createtopogeom('electorates_22_topo', 3, layer_id(findlayer('electorates_22', 'topo')), faces)
    -- Requires Postgis 3.2.0. (Not sure if this actually works.)

    update electorates_22 e
    set topo = createtopogeom('electorates_22_topo', 3, 1, faces)
    from (
        with e as (
          select e.name,
            f.face_id,
            st_area(st_intersection(e.geom, st_getfacegeometry('electorates_22_topo', f.face_id))) as area
          from electorates_22 e
            inner join electorates_22_topo.face f on e.geom && f.mbr
        )
        select topoelementarray_agg(ARRAY[e1.face_id, 3]) as faces,
          e1.name
        from e e1
        where area = (select max(area) from e e2 where e1.face_id = e2.face_id)
        group by e1.name
      ) t
    where e.name = t.name;
```

Turn that into a new geometry column!

```
    select addgeometrycolumn('public', 'electorates_22', 'new_geom', 3577, 'MULTIPOLYGON', 2);

    update electorates_22 set new_geom = topo::geometry;

    create index electorates_22_new_geom_idx on electorates_22 using gist(new_geom);
```

## 4. Create a table of booth locations.  @Incomplete: Below this heading. (Our next goal is to do the same thing for Voronoi diagrams of booths within districts.)

```
    create table booths_22(booth_id int, electorate_name varchar(32));

    select addgeometrycolumn('public', 'booths_22', 'geom', 3577, 'POINT', 2);

    update booths_22
    set geom =
```

## 5. Add another geometry column to the districts table: a MultiPoint of all its booths.

## 6. Create a Voronoi for each of the districts from the booth MultiPoint.

Create a new geometry column in the booths table. In that, re-associate each polygon in the district's Voronoi with its booth.

Still in the booths table, create a new geometry column clipping each Voronoi polygon by the district's topologically-validated shape.

## 7. Validate the topology of the final clipped column.

Create a topology layer for the column.

Create a new topology column in the booths table. It holds the topology face IDs for each booth---associating each booth with its polygons in the Voronoi.

## 8. This time, don't bother turning it into a new geometry column. This will stay in the query so that we can use `st_simplifypreservetopology` in the query!
