# Registry Index Format

A wrap registry index stores all data necessary for wrap's solver to calculate package dependency information
without having to go to the registry itself or into the package cache.

This data is known to the repository database, so it is very easy to produce and keep up to date. 

The format has to support incremental update to allow fetching changes from upstream registries as new
packages are added, without having to download the whole index again and again.

## Text format
 
The text format uses predictable ordering of lines and indentation for encoding the index. The format outline consists of:

```
format-version      := `format ` <int>  # format version number prefixed with word `format` followed by a space-separated integer.
compiler-version    := `<compiler-name> <semver>`  # compiler name [`elm`,`lamdera`,`guida`, etc. ] followed by a space-separated semver

package-def         := `package: <package-name>` `<version-block>`+
version-block       := <indent><version><indent><status><indent><license><indent><dependency-list>
dependency-list     := (<indent><package-name> <semver-range>)*                 # Always sorted highest semver first, and then in descending order

package-name        := `<string>/<string>`
verion              := `version: <semver>`
status              := `status: <valid|obsolete|missing|missing-deps>`
license             := `license: <SPDX-license-id|Custom>`
dependency-list     := `depencencies: (<dependency>)*`
dependency          := `<indent><package-name> <semver-range>`
semver-range        := `<semver> <= v < <semver>`

indent              := `"\n    "`
semver              := `<int>.<int>.<int>`
```

File shape visualized:

    format 2
    elm <elm-version>

    package: <author/name>
        version: <semver #higehest available>
        status: <valid|obsolete|missing|missing-deps>
        license: <SPDX-license-id|Custom>
        dependencies:
            <package-name>  <semver-range>
            <package-name>  <semver-range>
            <package-name>  <semver-range>

        version: <semver>
        status: <valid|obsolete|missing|missing-deps>
        license: <SPDX-license-id|Custom>
        dependencies:
            <package-name>  <semver_range>
            <package-name>  <semver_range>
            <package-name>  <semver_range>

        ...

    package: <author/name>
        version: <semver #higehest available>
        status: <valid|obsolete|missing|missing-deps>
        license: <SPDX-license-id|Custom>
        dependencies:
            <package-name>  <semver_range>
            <package-name>  <semver_range>
            <package-name>  <semver_range>
        
    ...



An example with values:

        format 2
        elm 0.19.1

        package: elm/core
            version: 1.0.5
            status: valid
            license: MIT
            dependencies:
                
            version: 1.0.4
            status: valid
            license: MIT
            dependencies:
            
            version: 1.0.3
            status: valid
            license: MIT
            dependencies:
                
            version: 1.0.2
            status: valid
            license: MIT
            dependencies:
            
            version: 1.0.1
            status: valid
            license: MIT
            dependencies:
            
            version: 1.0.0
            status: valid
            license: MIT
            dependencies:

        package: 1602/elm-feather
            version: 2.3.5
            status: valid
            license: BSD-3-Clause
            dependencies:
                elm/core        1.0.0 <= v < 2.0.0
                elm/html        1.0.0 <= v < 2.0.0
                elm/json        1.0.0 <= v < 2.0.0
                elm/svg         1.0.0 <= v < 2.0.0
                elm/virtual-dom 1.0.0 <= v < 2.0.0

            version: 2.3.4
            status: valid
            license: BSD-3-Clause
            dependencies:
                elm/core        1.0.0 <= v < 2.0.0
                elm/html        1.0.0 <= v < 2.0.0
                elm/json        1.0.0 <= v < 2.0.0
                elm/svg         1.0.0 <= v < 2.0.0
                elm/virtual-dom 1.0.0 <= v < 2.0.0

            version: 2.3.3
            status: valid
            license: BSD-3-Clause
            dependencies:
                elm/core        1.0.0 <= v < 2.0.0
                elm/html        1.0.0 <= v < 2.0.0
                elm/json        1.0.0 <= v < 2.0.0
                elm/svg         1.0.0 <= v < 2.0.0
                elm/virtual-dom 1.0.0 <= v < 2.0.0

            version: 2.3.2
            status: valid
            license: BSD-3-Clause
            dependencies:
                elm/core        1.0.0 <= v < 2.0.0
                elm/html        1.0.0 <= v < 2.0.0
                elm/json        1.0.0 <= v < 2.0.0
                elm/svg         1.0.0 <= v < 2.0.0
                elm/virtual-dom 1.0.0 <= v < 2.0.0


## DB Generation

We generate the index in a single query from the registry database:


```sql
create or replace function repository.get_full_index(p_compiler_version text, p_compiler text default 'elm')
returns text
language sql
begin atomic

WITH version_data AS (
    SELECT p.name
         , v.semver
         , v.license
         , v.status
         , array_agg(
               row(dp.name, semver_range_to_constraint(d.compatible_versions))
               ORDER BY dp.name
           ) FILTER (WHERE dp.name IS NOT NULL) AS deps
      FROM packages.package p
      JOIN packages.version v 
        ON v.package_id = p.id 
      JOIN packages.compiler_version cv 
        ON v.id = cv.version_id
      JOIN repository.compiler c
        ON c.id = cv.compiler_id AND c.compiler = p_compiler
       AND compatible_elm_versions @> p_compiler_version::semver_string::text
      LEFT JOIN packages.dependency d ON d.version_id = v.id
      LEFT JOIN packages.package dp ON dp.id = d.depended_on_package_id
     GROUP BY p.name, v.semver, v.license, v.status
     ORDER BY p.name
            , v.semver COLLATE "natural_sort" DESC
),
formatted_versions AS (
    SELECT name
         , format(E'    version: %s\n    status: %s\n    license: %s\n    dependencies:\n%s',
               semver,
               status,
               license,
               COALESCE(
                   (SELECT string_agg(format('        %s  %s', (d).f1, (d).f2), E'\n' ORDER BY (d).f1)
                    FROM unnest(deps) AS d(f1 text, f2 text)),
                   ''
               )
           ) AS version_block
      FROM version_data
),
formatted_packages AS (
    SELECT format(E'package: %s\n%s',
               name,
               string_agg(version_block, E'\n' ORDER BY version_block)
           ) AS package_block
      FROM formatted_versions
     GROUP BY name
     ORDER BY name
)
SELECT format(E'format 2\n%s %s\n\n%s\n',
            p_compiler,
            p_compiler_version,
            string_agg(package_block, E'\n\n' ORDER BY package_block)
       ) AS index_text
  FROM formatted_packages;
end;
```