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

    CREATE TABLE election (
      id INT PRIMARY KEY,
      name TEXT,
      date DATE
    );

    INSERT INTO election
    SELECT election_id AS id,
      (xpath('//*[local-name()=''EventName'']/text()', xmldata))[1]::TEXT AS name,
      (xpath('//*[local-name()=''Election''][*/@Id=''H'']//*[local-name()=''SingleDate'']/text()', xmldata))[1]::TEXT::DATE AS date
    FROM xml.eml_event;

Note that we will restrict all data to the lower house.


## Extract the electorates.

    CREATE TYPE australian_state AS ENUM ('ACT', 'NSW', 'NT', 'QLD', 'SA', 'TAS', 'VIC', 'WA');
    CREATE TYPE aec_demographic AS ENUM ('OuterMetropolitan', 'Rural', 'InnerMetropolitan', 'Provincial');

    CREATE TABLE district (
      election_id INT,
      id INT,
      name TEXT,
      short_code VARCHAR(4),
      state australian_state,
      demographic aec_demographic,
      enrolment INT,

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO district
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

    UPDATE district d
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
    WHERE d.election_id = t.election_id
      AND d.id = t.id;

Create a column for the electorates' boundaries.

    SELECT AddGeometryColumn('public', 'district', 'bounds', 3577, 'MULTIPOLYGON', 2);

Add the bounds for each year, clipping by the Australian coastline.

Recall that 2016 is a weird year.
We need to build the 2016 boundaries from the 2013 data and redistributions for three individual states: ACT, NSW and WA.

    --|Todo: Also clip out river polygons.

    UPDATE district d
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
    WHERE d.election_id = t.election_id
      AND d.name ILIKE t.elect_div;

    CREATE INDEX district_bounds_idx ON district USING gist(bounds);

Create a topology.

    SELECT CreateTopology('district_topo', 3577, 1);

    SELECT ST_CreateTopoGeo('district_topo', ST_Collect(bounds))
    FROM district;

That last command is slow.
It takes a few hours.

Create a topology layer to associate the faces in the newly created topology with the districts they represent.

    SELECT AddTopoGeometryColumn('district_topo', 'public', 'district', 'bounds_faces', 'MULTIPOLYGON');

Make a note of the layer ID returned by the above command and use it in the command below.
The command below creates a topogeometry column in the district table.
This topogeometry column holds the topology's face IDs for each district.

    --|Todo: Use FindLayer() to get the layer ID dynamically:
    --| SET bounds_faces = CreateTopoGeom('district_topo', 3, Layer_ID(FindLayer('district', 'topo')), faces)
    --| Requires Postgis 3.2.0. (Not sure if this actually works.)

    UPDATE district d
    SET bounds_faces = CreateTopoGeom('district_topo', 3, /*LAYER ID:*/1, faces)
    FROM (
        SELECT election_id, id, TopoElementArray_Agg(ARRAY[face_id, 3]) AS faces
        FROM (
            SELECT DISTINCT ON (election_id, face_id)
              d.election_id,
              d.id,
              f.face_id
            FROM district d
              INNER JOIN district_topo.face f ON d.bounds && f.mbr
            ORDER BY f.face_id,
              d.election_id,
              ST_Area(ST_Intersection(d.bounds, ST_GetFaceGeometry('district_topo', f.face_id))) DESC
          ) t
        GROUP BY election_id, id
      ) t
    WHERE d.election_id = t.election_id
      AND d.id = t.id;

In the above command, we look at all the faces in the topology and distribute them among the districts.
We want to assign each face to five districts: one district for each of the five elections.

In our boundary shapefiles, there is inconsistency from year to year about the coastline.
For example, between 2019 and 2022, the government remapped the north coast of Australia and included a lot more islands.
As a result, our topology contains a lot of unnecessary faces.
For instance, if an island was made bigger, there is one face for the original island and more faces for the extra parts added to it.
These extra faces are not interesting to us.
We only care about extra faces if they represent real changes in electoral boundaries due to redistrubutions.
Furthermore, we don't want the coastline changing from year to year.

In the above query, we try to assign *every* face in the topology to one district for each election---even if that election's shapefile didn't actually define the face in its own boundaries.
We achieve this with the `INNER JOIN ON d.bounds && f.mbr`, which creates a table with one row for every face/district combination where the bounding box of the face intersects with the district's bounding box.
In most cases, the join creates five rows for each face: one for each election.
This is the ideal case because there is no ambiguity about which district the face belongs to for each election.
When the inner join produces more than five rows, the `DISTINCT ON ... ORDER BY` assigns the face to whichever district it has the largest intersection with for that election.
If there are fewer than five rows, the face is dropped from the map for those elections where its bounding box does not intersect with that of any district.

In version 3.3.0 of Postgis, there is a topology function called `RemoveUnusedPrimitives`, which I think we could now run to remove the extra edges we don't care about from the topology.
I think we could achieve the same thing with our current version by creating a new topology from scratch out of our now-topologically-validated geometries.

Now get the outer edge IDs for each district.
We could make this a topogeometry column, but for now it's just an array of IDs.

    ALTER TABLE district ADD edge_ids INT[];

    UPDATE district d
    SET edge_ids = t.edge_ids
    FROM (
        SELECT t.election_id, t.id, array_agg(e.edge_id) AS edge_ids
        FROM (
            SELECT d.election_id, d.id, array_agg(r.element_id) AS face_ids
            FROM district d
              JOIN district_topo.relation r ON r.topogeo_id = (d.bounds_faces).id
            GROUP BY d.election_id, d.id
          ) t
          JOIN district_topo.edge e ON (
            (e.left_face = any(t.face_ids) AND NOT e.right_face = any(t.face_ids))
            OR (e.right_face = any(t.face_ids) AND NOT e.left_face = any(t.face_ids))
          )
        GROUP BY election_id, id
      ) t
    WHERE (d.election_id, d.id) = (t.election_id, t.id);


## Extract the booths.

    CREATE TABLE booth (
      election_id INT,
      id INT,
      name TEXT,
      address TEXT[],
      district_id INT,

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO booth
    SELECT election_id,
      (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Id', place))[1]::TEXT::INT AS id,
      (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Name', place))[1]::TEXT AS name,
      (xpath('//*[local-name()=''AddressLine'']/text()', place))::TEXT[] AS address,
      district_id
    FROM (
        SELECT election_id,
          unnest(xpath('/*/*/*[local-name()=''PollingPlace'']', district)) AS place,
          (xpath('/*/*[local-name()=''PollingDistrictIdentifier'']/@Id', district))[1]::TEXT::INT AS district_id
        FROM (
            SELECT election_id,
              unnest(xpath('/*/*/*[local-name()=''PollingDistrict'']', xmldata)) AS district
            FROM xml.aec_pollingdistricts
          ) t
      ) t;

Get the booth's location separately because many booths don't have lat/lon info but we still want them in the table.

    SELECT AddGeometryColumn('public', 'booth', 'location', 3577, 'POINT', 2);

    UPDATE booth b
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

    CREATE INDEX booth_location_idx ON booth USING gist(location);

## Extract the candidates and parties.

    CREATE TABLE candidate (
      election_id INT,
      id INT,
      first_name TEXT,
      last_name TEXT,
      party_id INT,
      district_id INT,

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO candidate
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
      district_id
    FROM (
        SELECT election_id,
          unnest(xpath('//*[local-name()=''Candidate'']', contest)) AS candidate,
          (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS district_id
        FROM (
            SELECT election_id,
              unnest(xpath('//*[local-name()=''Election''][*/@Id=''H'']//*[local-name()=''Contest'']', xmldata)) AS contest
            FROM xml.eml_candidates
          ) t
      ) t;

We give candidates a party ID of -1 when the AEC data notes that they are independent.
When the data says they are not independent, but they have no affiliation, we put -2.
There are only a few cases of the latter, and they're probably just mistakes, but we preserve the distinction anyway.

    CREATE TABLE party (
      election_id INT,
      id INT,
      name TEXT,
      short_code VARCHAR(4),
      colour INT CHECK (0 <= colour AND colour <= x'ffffff'::INT),

      PRIMARY KEY (election_id, id)
    );

    INSERT INTO party
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

    UPDATE party SET colour = x'bf1e2e'::INT WHERE short_code = 'AJP';
    UPDATE party SET colour = x'c31f2f'::INT WHERE short_code = 'ALP';
    UPDATE party SET colour = x'e41e0c'::INT WHERE short_code = 'ASP';
    UPDATE party SET colour = x'1725a1'::INT WHERE short_code = 'CDP';
    UPDATE party SET colour = x'fe7330'::INT WHERE short_code = 'CLP';
    UPDATE party SET colour = x'dd2a30'::INT WHERE short_code = 'CLR';
    UPDATE party SET colour = x'183a82'::INT WHERE short_code = 'FACN';
    UPDATE party SET colour = x'008c44'::INT WHERE short_code = 'GRN';
    UPDATE party SET colour = x'008c44'::INT WHERE short_code = 'GVIC';
    UPDATE party SET colour = x'ffdf00'::INT WHERE short_code = 'JLN';
    UPDATE party SET colour = x'e10e12'::INT WHERE short_code = 'KAP';
    UPDATE party SET colour = x'0e3b6d'::INT WHERE short_code = 'LDP';
    UPDATE party SET colour = x'0057a0'::INT WHERE short_code = 'LNP';
    UPDATE party SET colour = x'0057a0'::INT WHERE short_code = 'LNQ';
    UPDATE party SET colour = x'19488f'::INT WHERE short_code = 'LP';
    UPDATE party SET colour = x'00512d'::INT WHERE short_code = 'NP';
    UPDATE party SET colour = x'f36c21'::INT WHERE short_code = 'ON';
    UPDATE party SET colour = x'fe971a'::INT WHERE short_code = 'SPP';
    UPDATE party SET colour = x'feed01'::INT WHERE short_code = 'UAPP';
    UPDATE party SET colour = x'e46729'::INT WHERE short_code = 'XEN';


## Extract votes for the lower house.

    CREATE TYPE vote_count_type AS ENUM ('FP', '2CP');

    CREATE TABLE contest_vote (
      election_id INT,
      district_id INT,
      count_type vote_count_type,
      candidate_id INT,
      ballot_position INT,
      elected BOOLEAN,
      ordinary INT,
      absent INT,
      provisional INT,
      prepoll INT,
      postal INT,
      total INT,
      historic INT,

      PRIMARY KEY (election_id, district_id, count_type, candidate_id)
    );

    CREATE TABLE booth_vote (
      election_id INT,
      district_id INT,
      booth_id INT,
      count_type vote_count_type,
      candidate_id INT,
      total INT,

      PRIMARY KEY (election_id, district_id, booth_id, count_type, candidate_id)
    );

    INSERT INTO contest_vote
    SELECT election_id, district_id,
      (CASE WHEN ct = 'FirstPreferences' THEN 'FP' WHEN ct = 'TwoCandidatePreferred' THEN '2CP' END)::vote_count_type AS count_type,
      (xpath('/*/*[local-name()=''CandidateIdentifier'']/@Id', candidate))[1]::TEXT::INT AS candidate_id,
      (xpath('/*/*[local-name()=''BallotPosition'']/text()', candidate))[1]::TEXT::INT AS ballot_position,
      (xpath('/*/*[local-name()=''Elected'']/text()', candidate))[1]::TEXT::BOOLEAN AS elected,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Ordinary'']/text()', candidate))[1]::TEXT::INT AS ordinary,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Absent'']/text()', candidate))[1]::TEXT::INT AS absent,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Provisional'']/text()', candidate))[1]::TEXT::INT AS provisional,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''PrePoll'']/text()', candidate))[1]::TEXT::INT AS prepoll,
      (xpath('/*/*/*[local-name()=''Votes'' and @Type=''Postal'']/text()', candidate))[1]::TEXT::INT AS postal,
      (xpath('/*/*[local-name()=''Votes'']/text()', candidate))[1]::TEXT::INT AS total,
      (xpath('/*/*[local-name()=''Votes'']/@MatchedHistoric', candidate))[1]::TEXT::INT AS historic
    FROM (
        SELECT election_id, district_id,
          (xpath('name(/*)', fp_or_tcp))[1]::text AS ct,
          unnest(xpath('/*/*[local-name()=''Candidate'']', fp_or_tcp)) AS candidate
        FROM (
            SELECT election_id,
              (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS district_id,
              unnest(xpath('/*/*[local-name()=''FirstPreferences'' or local-name()=''TwoCandidatePreferred'']', contest)) AS fp_or_tcp
            FROM (
                SELECT election_id,
                  unnest(xpath('//*[local-name()=''House'']/*/*[local-name()=''Contest'']', xmldata)) AS contest
                FROM xml.aec_results
              ) t
          ) t
      ) t;

    INSERT INTO booth_vote
    SELECT election_id, district_id, booth_id,
      (CASE WHEN ct = 'FirstPreferences' THEN 'FP' WHEN ct = 'TwoCandidatePreferred' THEN '2CP' END)::vote_count_type AS count_type,
      (xpath('/*/*[local-name()=''CandidateIdentifier'']/@Id', candidate))[1]::TEXT::INT AS candidate_id,
      (xpath('/*/*[local-name()=''Votes'']/text()', candidate))[1]::TEXT::INT AS total
    FROM (
        SELECT election_id, district_id, booth_id,
          (xpath('name(/*)', fp_or_tcp))[1]::TEXT AS ct,
          unnest(xpath('/*/*[local-name()=''Candidate'']', fp_or_tcp)) AS candidate
        FROM (
            SELECT election_id, district_id,
              (xpath('/*/*[local-name()=''PollingPlaceIdentifier'']/@Id', place))[1]::TEXT::INT AS booth_id,
              unnest(xpath('/*/*[local-name()=''FirstPreferences'' or local-name()=''TwoCandidatePreferred'']', place)) AS fp_or_tcp
            FROM (
                SELECT election_id,
                  (xpath('/*/*[local-name()=''ContestIdentifier'']/@Id', contest))[1]::TEXT::INT AS district_id,
                  unnest(xpath('/*/*/*[local-name()=''PollingPlace'']', contest)) AS place
                FROM (
                    SELECT election_id,
                      unnest(xpath('//*[local-name()=''House'']/*/*[local-name()=''Contest'']', xmldata)) AS contest
                    FROM xml.aec_results
                  ) t
              ) t
          ) t
      ) t;
