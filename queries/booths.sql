with booth_nodes as (
  select unnest(
      xpath(
        '/aec:MediaFeed/aec:PollingDistrictList/aec:PollingDistrict/aec:PollingPlaces/aec:PollingPlace',
        xmldata,
        '{{"aec","http://www.aec.gov.au/xml/schema/mediafeed"}}'
      )
    ) as xmldata
  from polls_2022_federal
),
booths as (
  select (
      xpath(
        '/aec:PollingPlace/aec:PollingPlaceIdentifier/@Id',
        booth_nodes.xmldata,
        '{{"aec","http://www.aec.gov.au/xml/schema/mediafeed"}}'
      )
    ) [1]::text::integer as id,
    (
      xpath(
        '/aec:PollingPlace/aec:PollingPlaceIdentifier/@Name',
        booth_nodes.xmldata,
        '{{"aec","http://www.aec.gov.au/xml/schema/mediafeed"}}'
      )
    ) [1]::text as name,
    (
      xpath(
        '/aec:PollingPlace/eml:PhysicalLocation/eml:Address/xal:PostalServiceElements/xal:AddressLatitude/text()',
        booth_nodes.xmldata,
        '{{"aec","http://www.aec.gov.au/xml/schema/mediafeed"},{"eml","urn:oasis:names:tc:evs:schema:eml"},{"xal","urn:oasis:names:tc:ciq:xsdschema:xAL:2.0"}}'
      )
    ) [1]::text::numeric as lat,
    (
      xpath(
        '/aec:PollingPlace/eml:PhysicalLocation/eml:Address/xal:PostalServiceElements/xal:AddressLongitude/text()',
        booth_nodes.xmldata,
        '{{"aec","http://www.aec.gov.au/xml/schema/mediafeed"},{"eml","urn:oasis:names:tc:evs:schema:eml"},{"xal","urn:oasis:names:tc:ciq:xsdschema:xAL:2.0"}}'
      )
    ) [1]::text::numeric as lon
  from booth_nodes
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
voronoi as (
  select
    (st_dump(
      st_voronoipolygons(
        st_collect(
          point
        ),
        10.0
      )
    )).geom as polygon
  from projected
),
all_aust as (
  select st_makevalid(
    st_simplify(
      st_union(geom),
      1000
    )
  ) as geom from aust_coast
  where feat_code = 'mainland'
)
select
  --st_astext(
  st_asbinary(
    st_intersection(
      voronoi.polygon,
      all_aust.geom
    )
  ) as polygon
from voronoi
inner join all_aust on true
;
