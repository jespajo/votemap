with voronoi as (
  select booth_id::int,
    st_asbinary(st_collectionextract(geom, 3)) as polygon
  from (
      select booth_id,
        st_makevalid(st_snaptogrid(new_voronoi, $1::float)) as geom
      from booths_22
      where new_voronoi is not null
        and new_voronoi && st_setsrid(
          st_makebox2d(
            st_point($2::float, $3::float),
            st_point($4::float, $5::float)
          ),
          3577
        )
    ) t
)
select v.booth_id,
  v.polygon,
  l.fraction_of_votes,
  c.colour
from voronoi v
  left join (
    select r1.booth_id,
      candidate_id,
      num_votes::real / total_votes::real as fraction_of_votes
    from results_house_22 r1
      join (
        select booth_id,
          max(num_votes) as max_votes,
          sum(num_votes) as total_votes
        from results_house_22
        where vote_type = '2CP'
        group by booth_id
      ) r2 on (r1.booth_id = r2.booth_id)
    where vote_type = '2CP'
      and num_votes = max_votes
      and total_votes > 0
  ) l on v.booth_id = l.booth_id
  left join candidates_22 c on l.candidate_id = c.id
