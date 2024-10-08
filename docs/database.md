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

      PRIMARY KEY (election_id, id)
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

Create a topology.

    SELECT CreateTopology('electorates_topo', 3577);

    SELECT ST_CreateTopoGeo('electorates_topo', ST_Collect(bounds))
    FROM electorates;

That last command is slow.

Create a topology layer to associate the faces in the newly-created topology with the electorates they represent.

    SELECT AddTopoGeometryColumn('electorates_topo', 'public', 'electorates', 'bounds_topo', 'MULTIPOLYGON');

---

Make a note of the layer ID returned by the above command and use it in the command below.
The command below creates a new topology column in the electorates table.
This topology column holds the topology's face IDs for each electorate.

    --|Todo: Use FindLayer() to get the layer ID dynamically:
    --| SET bounds_topo = CreateTopoGeom('electorates_topo', 3, Layer_ID(FindLayer('electorates', 'topo')), faces)
    --| Requires Postgis 3.2.0. (Not sure if this actually works.)

    UPDATE electorates e
    SET bounds_topo = CreateTopoGeom('electorates_topo', 3, /*LAYER ID:*/1, faces)
    FROM (
        WITH e AS (
          SELECT e.election_id,
            e.name,
            f.face_id,
            ST_Area(ST_Intersection(e.bounds, ST_GetFaceGeometry('electorates_topo', f.face_id))) AS area
          FROM electorates e
            INNER JOIN electorates_topo.face f ON e.bounds && f.mbr
        )
        SELECT e1.election_id,
          e1.name,
          TopoElementArray_Agg(ARRAY[e1.face_id, 3]) AS faces
        FROM e e1
        WHERE area = (SELECT max(area) FROM e e2 WHERE e1.face_id = e2.face_id)
        GROUP BY e1.election_id, e1.name
      ) t
    WHERE e.election_id = t.election_id
      AND e.name = t.name;

Update the original geometries with the topologically validated ones.

---

**HAVEN'T RUN THIS**

    UPDATE electorates SET bounds = bounds_topo::geometry;

    DROP INDEX electorates_bounds_idx;
    CREATE INDEX electorates_bounds_idx ON electorates USING gist(bounds);

---

## Extract the booths.

    CREATE TABLE booths (
      election_id INT,
      id INT,
      name TEXT,
      address TEXT[],
      electorate_id INT,

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO booths
    SELECT election_id,
      (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Id', place))[1]::TEXT::INT AS id,
      (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Name', place))[1]::TEXT AS name,
      (xpath('//*[local-name()=''AddressLine'']/text()', place))::TEXT[] AS address,
      electorate_id
    FROM (
        SELECT election_id,
          unnest(xpath('/*/*/*[local-name()=''PollingPlace'']', district)) AS place,
          (xpath('/*/*[local-name()=''PollingDistrictIdentifier'']/@Id', district))[1]::TEXT::INT AS electorate_id
        FROM (
            SELECT election_id,
              unnest(xpath('/*/*/*[local-name()=''PollingDistrict'']', xmldata)) AS district
            FROM xml.aec_pollingdistricts
          ) t
      ) t;

Get the booth's location separately because many booths don't have lat/lon info but we still want them in the table.

    SELECT AddGeometryColumn('public', 'booths', 'location', 3577, 'POINT', 2);

    UPDATE booths b
    SET location = ST_Transform(ST_SetSRID(ST_Point(lon, lat), 4283), 3577)
    FROM (
        SELECT election_id,
          (xpath('/*/@Id', location))[1]::TEXT::INT AS id,
          (xpath('//*[local-name()=''AddressLatitude'']/text()', location))[1]::TEXT::NUMERIC AS lat,
          (xpath('//*[local-name()=''AddressLongitude'']/text()', location))[1]::TEXT::NUMERIC AS lon
        FROM (
            SELECT election_id,
              unnest(xpath('//*[local-name()=''PhysicalLocation'']', xmldata)) AS location
            FROM xml.aec_pollingdistricts
          ) t
      ) t
    WHERE (t.lat IS NOT NULL AND t.lon IS NOT NULL)
      AND (b.election_id = t.election_id AND b.id = t.id);

    CREATE INDEX booths_location_idx ON booths USING gist(location);

## Extract the candidates and parties.

    CREATE TABLE candidates (
      election_id INT,
      id INT,
      first_name TEXT,
      last_name TEXT,
      party_id INT,
      electorate_id INT,

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO candidates
    SELECT election_id,
      (xpath('/*/*[local-name()=''CandidateIdentifier'']/@Id', candidate))[1]::TEXT::INT AS id,
      COALESCE(
          (xpath('//*[local-name()=''FirstName'' and @Type=''BallotPaper'']/text()', candidate))[1]::TEXT,
          (xpath('//*[local-name()=''FirstName'']/text()', candidate))[1]::TEXT
        ) AS first_name,
      (xpath('//*[local-name()=''LastName'']/text()', candidate))[1]::text AS last_name,
      COALESCE(CASE
          WHEN (xpath('/*/@Independent', candidate))[1]::TEXT::BOOLEAN THEN -1
          ELSE (xpath('//*[local-name()=''AffiliationIdentifier'']/@Id', candidate))[1]::TEXT::INT
        END, -2) AS party_id,
      electorate_id
    FROM (
        SELECT election_id,
          unnest(xpath('//*[local-name()=''Candidate'']', contest)) AS candidate,
          (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS electorate_id
        FROM (
            SELECT election_id,
              unnest(xpath('//*[local-name()=''Election''][*/@Id=''H'']//*[local-name()=''Contest'']', xmldata)) AS contest
            FROM xml.eml_candidates
          ) t
      ) t;

We give candidates a party ID of -1 when the AEC data notes that they are independent.
When the data says they are not independent, but they have no affiliation, we put -2.
There are only a few cases of the latter, and they're probably just mistakes, but we preserve the distinction anyway.

    CREATE TABLE parties (
      election_id INT,
      id INT,
      name TEXT,
      short_code VARCHAR(4),
      colour INT CHECK (0 <= colour AND colour <= x'ffffff'::INT),

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO parties
    SELECT DISTINCT ON (election_id, id)
      election_id,
      (xpath('/*/@Id', identifier))[1]::TEXT::INT AS id,
      (xpath('/*/*[local-name()=''RegisteredName'']/text()', identifier))[1]::TEXT AS name,
      (xpath('/*/@ShortCode', identifier)) [1]::TEXT AS short_code,
      0 AS colour
    FROM (
        SELECT election_id,
          unnest(xpath('//*[local-name()=''AffiliationIdentifier'']', xmldata)) AS identifier
        FROM xml.eml_candidates
      ) t;

The `DISTINCT ON` means we use the first name that happens to appear for each party, for each election.
There is variation in the party names associated with the same ID even within elections.

Add some colours.

    UPDATE parties SET colour = x'bf1e2e'::INT WHERE short_code = 'AJP';
    UPDATE parties SET colour = x'c31f2f'::INT WHERE short_code = 'ALP';
    UPDATE parties SET colour = x'e41e0c'::INT WHERE short_code = 'ASP';
    UPDATE parties SET colour = x'1725a1'::INT WHERE short_code = 'CDP';
    UPDATE parties SET colour = x'fe7330'::INT WHERE short_code = 'CLP';
    UPDATE parties SET colour = x'dd2a30'::INT WHERE short_code = 'CLR';
    UPDATE parties SET colour = x'183a82'::INT WHERE short_code = 'FACN';
    UPDATE parties SET colour = x'008c44'::INT WHERE short_code = 'GRN';
    UPDATE parties SET colour = x'008c44'::INT WHERE short_code = 'GVIC';
    UPDATE parties SET colour = x'ffdf00'::INT WHERE short_code = 'JLN';
    UPDATE parties SET colour = x'e10e12'::INT WHERE short_code = 'KAP';
    UPDATE parties SET colour = x'0e3b6d'::INT WHERE short_code = 'LDP';
    UPDATE parties SET colour = x'0057a0'::INT WHERE short_code = 'LNP';
    UPDATE parties SET colour = x'0057a0'::INT WHERE short_code = 'LNQ';
    UPDATE parties SET colour = x'19488f'::INT WHERE short_code = 'LP';
    UPDATE parties SET colour = x'00512d'::INT WHERE short_code = 'NP';
    UPDATE parties SET colour = x'f36c21'::INT WHERE short_code = 'ON';
    UPDATE parties SET colour = x'fe971a'::INT WHERE short_code = 'SPP';
    UPDATE parties SET colour = x'feed01'::INT WHERE short_code = 'UAPP';
    UPDATE parties SET colour = x'e46729'::INT WHERE short_code = 'XEN';


## Extract votes for the lower house.

    CREATE TYPE vote_count_type AS ENUM ('FP', '2CP');

    CREATE TABLE contest_votes (
      election_id INT,
      electorate_id INT,
      count_type vote_count_type,
      candidate_id INT,
      ballot_position INT,
      elected BOOLEAN,
      ordinary_votes INT,
      absent_votes INT,
      provisional_votes INT,
      prepoll_votes INT,
      postal_votes INT,
      total_votes INT,
      historic_votes INT,

      PRIMARY KEY (election_id, electorate_id, count_type, candidate_id)
    );

    CREATE TABLE booth_votes (
      election_id INT,
      electorate_id INT,
      booth_id INT,
      count_type vote_count_type,
      candidate_id INT,
      votes INT,

      PRIMARY KEY (election_id, electorate_id, booth_id, count_type, candidate_id)
    );

    INSERT INTO contest_votes
    SELECT election_id, electorate_id,
      (CASE WHEN ct = 'FirstPreferences' THEN 'FP' WHEN ct = 'TwoCandidatePreferred' THEN '2CP' END)::vote_count_type AS count_type,
      (xpath('/*/*[local-name()=''CandidateIdentifier'']/@Id', candidate))[1]::TEXT::INT AS candidate_id,
      (xpath('/*/*[local-name()=''BallotPosition'']/text()', candidate))[1]::TEXT::INT AS ballot_position,
      (xpath('/*/*[local-name()=''Elected'']/text()', candidate))[1]::TEXT::BOOLEAN AS elected,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Ordinary'']/text()', candidate))[1]::TEXT::INT AS ordinary_votes,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Absent'']/text()', candidate))[1]::TEXT::INT AS absent_votes,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Provisional'']/text()', candidate))[1]::TEXT::INT AS provisional_votes,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''PrePoll'']/text()', candidate))[1]::TEXT::INT AS prepoll_votes,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Postal'']/text()', candidate))[1]::TEXT::INT AS postal_votes,
      (xpath('/*/*[local-name()=''Votes'']/text()', candidate))[1]::TEXT::INT AS total_votes,
      (xpath('/*/*[local-name()=''Votes'']/@MatchedHistoric', candidate))[1]::TEXT::INT AS historic_votes
    FROM (
        SELECT election_id, electorate_id,
          (xpath('name(/*)', fp_or_tcp))[1]::text AS ct,
          unnest(xpath('/*/*[local-name()=''Candidate'']', fp_or_tcp)) AS candidate
        FROM (
            SELECT election_id,
              (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS electorate_id,
              unnest(xpath('/*/*[local-name()=''FirstPreferences'' or local-name()=''TwoCandidatePreferred'']', contest)) AS fp_or_tcp
            FROM (
                SELECT election_id,
                  unnest(xpath('//*[local-name()=''House'']/*/*[local-name()=''Contest'']', xmldata)) AS contest
                FROM xml.aec_results
              ) t
          ) t
      ) t;

    INSERT INTO booth_votes
    SELECT election_id, electorate_id, booth_id,
      (CASE WHEN ct = 'FirstPreferences' THEN 'FP' WHEN ct = 'TwoCandidatePreferred' THEN '2CP' END)::vote_count_type AS count_type,
      (xpath('/*/*[local-name()=''CandidateIdentifier'']/@Id', candidate))[1]::TEXT::INT AS candidate_id,
      (xpath('/*/*[local-name()=''Votes'']/text()', candidate))[1]::TEXT::INT AS votes
    FROM (
        SELECT election_id, electorate_id, booth_id,
          (xpath('name(/*)', fp_or_tcp))[1]::TEXT AS ct,
          unnest(xpath('/*/*[local-name()=''Candidate'']', fp_or_tcp)) AS candidate
        FROM (
            SELECT election_id, electorate_id,
              (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Id', place))[1]::TEXT::INT AS booth_id,
              unnest(xpath('/*/*[local-name()=''FirstPreferences'' or local-name()=''TwoCandidatePreferred'']', place)) AS fp_or_tcp
            FROM (
                SELECT election_id,
                  (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS electorate_id,
                  unnest(xpath('/*/*/*[local-name()=''PollingPlace'']', contest)) AS place
                FROM (
                    SELECT election_id,
                      unnest(xpath('//*[local-name()=''House'']/*/*[local-name()=''Contest'']', xmldata)) AS contest
                    FROM xml.aec_results
                  ) t
              ) t
          ) t
      ) t;
