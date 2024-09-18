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
    create table electorates_22 (
        name varchar(32) primary key
      );

    insert into electorates_22
    select elect_div as name
    from shp.bounds_22;
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
          st_intersection(bounds.geom, land.geom, 3.0) as clipped
        from shp.bounds_22 bounds
          join (
            select st_union(geom) as geom
            from shp.coast
            where feat_code in ('mainland', 'island')
          ) land on st_intersects(bounds.geom, land.geom)
      ) t
    where e.name = t.elect_div;

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
    -- |Todo: Use findlayer() to get the layer ID dynamically:
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

## 4. Add XML data to the database.

```
    create table xml.aec_pollingdistricts (
        election_id int primary key,
        xmldata     xml
      );
```

From the shell:
```
    ELECTION_ID=27966
    printf "insert into xml.aec_pollingdistricts values ($ELECTION_ID, '$(
        xmllint --noblanks /tmp/aec-mediafeed-pollingdistricts.xml | sed "1d;s/'/''/g"
    )');" | pq
```

## 5. Create a table of booth locations.

```
    create table booths_22 (
        booth_id        int,          --|Todo: Make this the primary key.
        booth_name      text,
        electorate_name varchar(32)
      );

    select addgeometrycolumn('public', 'booths_22', 'geom', 3577, 'POINT', 2);

    insert into booths_22
    select id as booth_id,
      name as booth_name,
      electorate_name,
      st_transform(st_setsrid(st_point(lon, lat), 4283), 3577) as geom
    from (
        select electorate_name,
          (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Id',    polling_place))[1]::text::integer as id,
          (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Name',  polling_place))[1]::text          as name,
          (xpath('/*/*/*/*/*[local-name()=''AddressLatitude'']/text()',  polling_place))[1]::text::numeric as lat,
          (xpath('/*/*/*/*/*[local-name()=''AddressLongitude'']/text()', polling_place))[1]::text::numeric as lon
        from (
            select
              (xpath('/*/*/*[local-name()=''Name'']/text()', district))[1]::text as electorate_name,
              unnest(xpath('/*/*/*[local-name()=''PollingPlace'']', district))   as polling_place
            from (
                select unnest(xpath('/*/*/*[local-name()=''PollingDistrict'']', xmldata)) as district
                from xml.aec_pollingdistricts
                where election_id = 27966
              ) t
          ) t
      ) t
    where lat is not null
      and lon is not null;

    create index booths_22_geom_idx on booths_22 using gist(geom);
```

## 6. Add another geometry column to the districts table: a MultiPolygon Voronoi diagram of the polling places in that district.

```
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
```

We restrict our set of booths to ones where the XML and spatial data agree---that is, the coordinates for the booth fall into the boundaries of the booth's district.

Note that the `booth_voronoi` multipolygons are huge overlapping rectangles---not yet clipped by each district's boundaries.

## 7.  Add a geometry column to the booths table to associate booths with polygons from the Voronoi diagrams.

Only a subset of booths will have a value in their `voronoi` field.
If present, the value is the polygon from the overall Voronoi map associated with that booth.
This polygon has been clipped by the boundaries of the booth's district.

Currently we ignore a few cases in our data:
- Booths whose position is, according to our spatial data, not in the correct electoral district.
- Multiple booths with the same location. We just take the first one. This is especially common with pre-polling booths always being a separate booth with the same location.

```
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
```

## 8. Create a topogeometry from the Voronoi diagram.

```
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
```

## 9. Create a geometry from the topogeometry.

This is just so we can make use of a spatial index.

```
    select addgeometrycolumn('public', 'booths_22', 'new_voronoi', 3577, 'MULTIPOLYGON', 2);

    update booths_22
    set new_voronoi = multipolygon
    from (
        select booth_id, topo as multipolygon from booths_22
      ) t
    where booths_22.booth_id = t.booth_id;

    create index booths_22_new_voronoi_idx on booths_22 using gist(new_voronoi);
```

## 10. Create a table of candidates.

First the XML table:

```
    create table xml.eml_candidates (
        election_id int primary key,
        xmldata     xml
      );
```

Then populate that from the shell:

```
    DATA_DIR=/home/jpj/src/webgl/reference/votemap-1-data/aec
    ELECTION_ID=27966
    printf "insert into xml.eml_candidates values ($ELECTION_ID, '$(
        xmllint --noblanks $DATA_DIR/$ELECTION_ID/eml-230-candidates.xml | sed "1d;s/'/''/g"
    )');" | pq
```

Now the actual SQL table:

```
    create table candidates_22 (
        id          int     primary key,
        name        text,
        independent boolean,
        party_id    int,
        party_name  text,
        colour      int     check (0 <= colour and colour <= x'ffffff'::int)
      );

    insert into candidates_22
    select
      (xpath('/*/@Id',                                       candidate_identifier))[1]::text::int   as id,
      (xpath('/*/*[local-name()=''CandidateName'']/text()',  candidate_identifier))[1]::text        as name,
      independent::text::boolean                                                                    as independent,
      (xpath('/*/@Id',                                       affiliation_identifier))[1]::text::int as party_id,
      (xpath('/*/*[local-name()=''RegisteredName'']/text()', affiliation_identifier))[1]::text      as party_name,
      '0'::int                                                                                      as colour
    from (
        select
          (xpath('/*/@Independent',                                candidate))[1] as independent,
          (xpath('/*/*[local-name()=''CandidateIdentifier'']',     candidate))[1] as candidate_identifier,
          (xpath('/*/*/*[local-name()=''AffiliationIdentifier'']', candidate))[1] as affiliation_identifier
        from (
            select (unnest(xpath('/*/*/*/*/*[local-name()=''Candidate'']', xmldata))) as candidate
            from xml.eml_candidates
          ) t
      ) t;
```

Add a splash of wrong, temporary colours.

```
    update candidates_22 set colour = x'ff0000'::int where party_name like '%Labor%';
    update candidates_22 set colour = x'00ff00'::int where party_name like '%Green%';
    update candidates_22 set colour = x'0000ff'::int where party_name like '%Liberal%';
```
