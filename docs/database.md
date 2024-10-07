## Import the shapefiles with coastline and district data.

Create a schema for shapefiles.

    CREATE SCHEMA shp;

Import the Australian coast polygons.

    shp2pgsql -D -I -s 4283:3577 maps/coast/australia/cstauscd_r shp.coast | pq

This assumes the coordinate reference system is ESPG:4283 (GDA94 using lon/lat).
We're projecting them to ESPG:3577 (Australian Albers using metres).

The file argument is the names of the shp, prj, etc. files without the extensions.

Import the electoral district boundaries.

    # 2010 federal election
    shp2pgsql -D -I -s 4283:3577 maps/aec/15508/boundaries shp.boundaries_2010 | pq

    # 2013 federal election
    shp2pgsql -D -I -s 4283:3577 maps/aec/17496/boundaries shp.boundaries_2013 | pq

    # 2016 federal election
    shp2pgsql -D -I -s 4283:3577 maps/aec/20499/act/boundaries shp.boundaries_2016_act | pq
    shp2pgsql -D -I -s 4283:3577 maps/aec/20499/nsw/boundaries shp.boundaries_2016_nsw | pq
    shp2pgsql -D -I -s 4283:3577 maps/aec/20499/wa/boundaries  shp.boundaries_2016_wa  | pq

    # 2019 federal election
    shp2pgsql -D -I -s 4283:3577 maps/aec/24310/boundaries shp.boundaries_2019 | pq

    # 2022 federal election
    shp2pgsql -D -I -s 4283:3577 maps/aec/27966/boundaries shp.boundaries_2022 | pq

The 2016 boundaries are different.
The government doesn't provide them as a single shapefile.
We can only import the shapefiles for the individual states that had redistributions---ACT, NSW and WA.
We'll have to merge them with the 2013 shapes for the other states.


## Import the XML data.

Set up the schema and tables.

    CREATE SCHEMA xml;

    CREATE TABLE xml.aec_pollingdistricts (election_id INT PRIMARY KEY, xmldata XML);
    CREATE TABLE xml.aec_results          (election_id INT PRIMARY KEY, xmldata XML);
    CREATE TABLE xml.eml_event            (election_id INT PRIMARY KEY, xmldata XML);
    CREATE TABLE xml.eml_candidates       (election_id INT PRIMARY KEY, xmldata XML);

Import from the shell.

    for election_id in 15508 17496 20499 24310 27966; do
        printf "INSERT INTO xml.aec_pollingdistricts VALUES ($election_id, '$(
            xmllint --noblanks aec/$election_id/aec-mediafeed-pollingdistricts.xml | sed "1d;s/'/''/g"
        )');" | pq

        printf "INSERT INTO xml.aec_results VALUES ($election_id, '$(
            xmllint --noblanks aec/$election_id/aec-mediafeed-results-detailed-light.xml | sed "1d;s/'/''/g"
        )');" | pq

        printf "INSERT INTO xml.eml_event VALUES ($election_id, '$(
            xmllint --noblanks aec/$election_id/eml-110-event.xml | sed "1d;s/'/''/g"
        )');" | pq
        
        printf "INSERT INTO xml.eml_candidates VALUES ($election_id, '$(
            xmllint --noblanks aec/$election_id/eml-230-candidates.xml | sed "1d;s/'/''/g"
        )');" | pq
    done


## Extract the elections into a table.

    CREATE TABLE elections (
      id INT PRIMARY KEY,
      name TEXT,
      date DATE
    );

    INSERT INTO elections
    SELECT election_id AS id,
      (xpath('//*[local-name()=''EventName'']/text()', xmldata))[1]::TEXT AS name,
      (xpath('//*[local-name()=''Election''][*/@Id=''H'']//*[local-name()=''SingleDate'']/text()', xmldata))[1]::TEXT::DATE AS date
    FROM xml.eml_event;

Note that we will restrict all data to the lower house.


## Extract the electorates.

    CREATE TYPE australian_state AS ENUM ('ACT', 'NSW', 'NT', 'QLD', 'SA', 'TAS', 'VIC', 'WA');
    CREATE TYPE aec_demographic AS ENUM ('OuterMetropolitan', 'Rural', 'InnerMetropolitan', 'Provincial');

    CREATE TABLE electorates (
      election_id INT,
      id INT,
      name TEXT,
      short_code VARCHAR(4),
      state australian_state,
      demographic aec_demographic,
      enrolment INT,
      PRIMARY KEY (id, election_id)
    );

    INSERT INTO electorates
    SELECT election_id,
      (xpath('/*/@Id', identifier)) [1]::TEXT::INT AS id,
      (xpath('/*/*[local-name()=''Name'']/text()', identifier))[1]::TEXT AS name,
      (xpath('/*/@ShortCode', identifier)) [1]::TEXT AS short_code,
      (xpath('/*/*[local-name()=''StateIdentifier'']/@Id', identifier))[1]::TEXT::australian_state AS state,
      (xpath('/*/*[local-name()=''Demographic'']/text()', district))[1]::TEXT::aec_demographic AS demographic
    FROM (
        SELECT *,
          (xpath('/*/*[local-name()=''PollingDistrictIdentifier'']', district))[1] AS identifier
        FROM (
            SELECT election_id,
              unnest(xpath('//*[local-name()=''PollingDistrict'']', xmldata)) AS district
            FROM xml.aec_pollingdistricts
          ) t
      ) t;

Update the enrolment column separately because we have to get it from a different XML source.

    UPDATE electorates e
    SET enrolment = t.enrolment
    FROM (
        SELECT election_id,
          (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS id,
          (xpath('/*/*[local-name()=''Enrolment'']/text()', contest))[1]::TEXT::INT AS enrolment
        FROM (
            SELECT election_id,
              unnest(xpath('//*[local-name()=''Election''][*/@Id=''H'']//*[local-name()=''Contest'']', xmldata)) AS contest
            FROM xml.aec_results
          ) t
      ) t
    WHERE e.election_id = t.election_id
      AND e.id = t.id;

Create a column for the electorates' boundaries.

    SELECT AddGeometryColumn('public', 'electorates', 'bounds', 3577, 'MULTIPOLYGON', 2);

Add the bounds for each year, clipping by the Australian coastline.

Recall that 2016 is a weird year.
We need to build the 2016 boundaries from the 2013 data and redistributions for three individual states: ACT, NSW and WA.

    UPDATE electorates e
    SET bounds = ST_Force2D(ST_Multi(ST_CollectionExtract(clipped, 3)))
    FROM (
        SELECT election_id,
          elect_div,
          ST_Intersection(b.geom, c.geom, 3.0) AS clipped
        FROM (
            SELECT 15508 AS election_id, elect_div, geom
            FROM shp.boundaries_2010
           UNION ALL
            SELECT 17496 AS election_id, elect_div, geom
            FROM shp.boundaries_2013
           UNION ALL
            SELECT 20499 AS election_id, elect_div, geom
            FROM (
                SELECT elect_div, geom
                FROM shp.boundaries_2013
                WHERE state NOT IN ('ACT', 'NSW', 'WA')
               UNION ALL
                SELECT elect_div, geom
                FROM shp.boundaries_2016_act
               UNION ALL
                SELECT elect_div, geom
                FROM shp.boundaries_2016_nsw
               UNION ALL
                SELECT elect_div, geom
                FROM shp.boundaries_2016_wa
              ) t
           UNION ALL
            SELECT 24310 AS election_id, elect_div, geom
            FROM shp.boundaries_2019
           UNION ALL
            SELECT 27966 AS election_id, elect_div, geom
            FROM shp.boundaries_2022
          ) b
          JOIN (
            SELECT ST_Union(geom) AS geom
            FROM shp.coast
            WHERE feat_code IN ('mainland', 'island')
          ) c ON b.geom && c.geom
      ) t
    WHERE e.election_id = t.election_id
      AND e.name ILIKE t.elect_div;

    CREATE INDEX electorates_bounds_idx ON electorates USING gist(bounds);

---

# PLACEHOLDER NOTES BELOW HERE

## Extract the booths.

    CREATE TABLE booths (
        election_id INT,
        id INT,
        name TEXT,
        address TEXT[],
        
        
        PRIMARY KEY (id, election_id)
    );

    SELECT AddGeometryColumn('public', 'booths', 'geom', 3577, 'POINT', 2);


## Extract the candidates and parties.

    candidates
      (id, first_name, last_name, party_id)

    parties
      (id, code, name, colour)


## Extract the votes.

Create the following types in the database:
    count_type (2CP, FP)

    contest_votes
      (election_id, contest_id, count_type, candidate_id, ballot_position, elected, ordinary_votes, absent_votes, provisional_votes, prepoll_votes, postal_votes, total_votes)

    booth_votes
      (election_id, contest_id, booth_id, count_type, candidate_id, votes)
