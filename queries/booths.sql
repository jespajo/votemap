--
-- This venerable query is @Deprecated, but, for now, here for reference.
--
with booth_nodes as (
  select unnest(
      xpath('/aec:MediaFeed/aec:PollingDistrictList/aec:PollingDistrict/aec:PollingPlaces/aec:PollingPlace', xmldata, $1)
    ) as xmldata
  from polls_2022_federal
),
booths as (
  select
    (xpath('/aec:PollingPlace/aec:PollingPlaceIdentifier/@Id',       t.xmldata, $1))[1]::text::integer as id,
    (xpath('/aec:PollingPlace/aec:PollingPlaceIdentifier/@Name',     t.xmldata, $1))[1]::text          as name,
    (xpath('/xal:PostalServiceElements/xal:AddressLatitude/text()',  t.address, $1))[1]::text::numeric as lat,
    (xpath('/xal:PostalServiceElements/xal:AddressLongitude/text()', t.address, $1))[1]::text::numeric as lon
  from (
      select xmldata,
        (xpath('/aec:PollingPlace/eml:PhysicalLocation/eml:Address/xal:PostalServiceElements', xmldata, $1))[1] as address
      from booth_nodes
    ) t
),
projected as (
  select
    st_transform(
      st_setsrid(st_point(lon, lat), 4283),
      3577
    ) as point
  from booths
  where lat is not null
    and lon is not null
),
district as (
  -- @Todo: This is where we should be clipping the districts by the coastline.
  select 
    f.*,
    st_makevalid(
      st_simplifypreservetopology( -- @Bug: This doesn't actually seem to preserve the topology. Maybe we need to aggregate the districts first for this to work?
        st_force2d(f.geom),
        100.0
      )
    ) as bounds,
    st_collect(p.point) as points
  from federal_boundaries_2022 f
  join projected p
    on st_contains(f.geom, p.point)
  group by f.gid
),
voronoi as (
  select
    (st_dump(st_voronoipolygons(
      district.points,
      100.0
    ))).geom as polygon,
  district.bounds
  from district
),
coast as (
  select
    st_makevalid(
      st_simplifypreservetopology(
        st_union(
          geom
        ),
        100.0
      )
    ) as polygon
  from aust_coast
  where feat_code = 'mainland'
  or (feat_code = 'island' and area > 0.01)
),
clipped as (
  select
    st_makevalid(st_intersection(
      st_makevalid(st_collectionextract(st_intersection(
        voronoi.polygon,
        voronoi.bounds,
        10.0
      ), 3)),
      coast.polygon,
      10.0
    )) as geom
  from voronoi
  join coast on true
)
select
  st_asbinary(
    st_makevalid(
      st_collectionextract(
        clipped.geom,
        3 -- return polygons only
      )
    )
  ) as polygon
from clipped;
