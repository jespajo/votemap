--
--  START TRANSACTION;
--  SELECT DropTopology('federal_bounds_2022_topo');
--  SELECT CreateTopology('federal_bounds_2022_topo', 3577);
--  SELECT ST_CreateTopoGeo(
--      'federal_bounds_2022_topo',
--      ST_Collect(ST_Force2D(geom))
--    )
--  FROM (
--      SELECT ST_Intersection(district.geom, land.geom, 3.0) AS geom
--      FROM (
--          SELECT ST_Union(geom) AS geom
--          FROM aust_coast
--          WHERE feat_code IN ('mainland', 'island')
--        ) land
--        JOIN federal_boundaries_2022 district
--          ON ST_Intersects(district.geom, land.geom)
--    ) t;
--  COMMIT;
--
select st_asbinary(t.geom) as path
from (
    select st_simplifypreservetopology(geom, $1::float) as geom
    from federal_bounds_2022_topo.edge_data
  ) t;
