#!/system/bin/sh

MODDIR=${0%/*}
MARKER="/dev/.murong_hmbird_restore_done"
JSON_ROOT="$MODDIR/hmbird"
DB_PRIMARY="/data/data/com.oplus.cosa/databases/db_game_database"
DB_SECONDARY="/data/user_de/0/com.oplus.cosa/databases/db_game_database"
APP_PACKAGE="com.murong.diaodu"
META_SQL="/dev/.murong_hmbird_json_meta_$$.sql"
WORK_SQL="/dev/.murong_hmbird_json_work_$$.sql"

umask 077

cleanup_temp_sql() {
    rm -f "$META_SQL" "$WORK_SQL" 2>/dev/null
    return 0
}

trap cleanup_temp_sql 0
trap 'exit 1' HUP INT TERM

resolve_soc() {
    for prop_key in ro.soc.model ro.boot.chipname ro.board.platform ro.hardware; do
        value=$(getprop "$prop_key" 2>/dev/null)
        upper=$(echo "$value" | tr '[:lower:]' '[:upper:]')
        case "$upper" in
            *SM8650P*|*SM8650*|*PINEAPPLE*)
                echo "SM8650"
                return 0
                ;;
            *SM8750P*|*SM8750*)
                echo "SM8750"
                return 0
                ;;
            *SM8850P*|*SM8850*)
                echo "SM8850"
                return 0
                ;;
            *SM8845P*|*SM8845*)
                echo "SM8845"
                return 0
                ;;
            *MT6991*)
                echo "MT6991"
                return 0
                ;;
            *MT6993*)
                echo "MT6993"
                return 0
                ;;
            *ICELAND*)
                echo "iceland"
                return 0
                ;;
        esac
    done
    return 1
}

resolve_sqlite3() {
    for path in "$MODDIR/bin/sqlite3" "/data/user/0/$APP_PACKAGE/files/tool/sqlite3" "/data/data/$APP_PACKAGE/files/tool/sqlite3"; do
        [ -x "$path" ] && {
            echo "$path"
            return 0
        }
    done
    return 1
}

resolve_database() {
    [ -f "$DB_PRIMARY" ] && {
        echo "$DB_PRIMARY"
        return 0
    }
    [ -f "$DB_SECONDARY" ] && {
        echo "$DB_SECONDARY"
        return 0
    }
    return 1
}

sqlite_has_json1() {
    cat <<'EOF' | "$SQLITE3_BIN" :memory: >/dev/null 2>&1
select json_extract('{"a":1}','$.a');
EOF
}

write_json_doc_sql() {
    sql_path=$1
    json_path=$2
    {
        printf 'CREATE TEMP TABLE __json_doc(doc TEXT);\n'
        printf "INSERT INTO __json_doc(doc) VALUES ('"
        tr -d '\000' < "$json_path" | tr '\r\n' '  ' | sed "s/'/''/g"
        printf "');\n"
    } > "$sql_path"
}

apply_json_dir() {
    dir_path=$1
    sqlite_has_json1 || return 1
    package_path='$.package_name'
    root_path='$'

    for json_file in "$dir_path"/*.json; do
        [ -f "$json_file" ] || continue
        found_count=$((found_count + 1))
        cleanup_temp_sql

        write_json_doc_sql "$META_SQL" "$json_file"
        printf "select json_extract(doc,'%s') from __json_doc;\n" "$package_path" >> "$META_SQL"
        package_name=$("$SQLITE3_BIN" :memory: < "$META_SQL" 2>/dev/null | tr -d '\r' | tail -n 1)
        if [ -z "$package_name" ]; then
            cleanup_temp_sql
            continue
        fi
        package_sql=$(printf '%s' "$package_name" | sed "s/'/''/g")

        write_json_doc_sql "$META_SQL" "$json_file"
        printf "select key from json_each((select doc from __json_doc)) where path='%s';\n" "$root_path" >> "$META_SQL"
        key_list=$("$SQLITE3_BIN" :memory: < "$META_SQL" 2>/dev/null | tr -d '\r')

        columns=""
        values=""
        old_ifs=$IFS
        IFS='
'
        for json_key in $key_list; do
            [ -n "$json_key" ] || continue
            [ -n "$columns" ] && {
                columns="$columns, "
                values="$values, "
            }
            columns="$columns\"$json_key\""
            path_expr=$(printf '$.%s' "$json_key")
            values="$values json_extract(doc, '$path_expr')"
        done
        IFS=$old_ifs

        if [ -z "$columns" ]; then
            cleanup_temp_sql
            continue
        fi

        write_json_doc_sql "$WORK_SQL" "$json_file"
        cat <<EOF >> "$WORK_SQL"
BEGIN TRANSACTION;
DELETE FROM PackageConfigBean WHERE Package_Name='$package_sql';
INSERT INTO PackageConfigBean ($columns) SELECT $values FROM __json_doc;
COMMIT;
EOF
        "$SQLITE3_BIN" "$DATABASE_PATH" < "$WORK_SQL" >/dev/null 2>&1 && success_count=$((success_count + 1))
        cleanup_temp_sql
    done

    return 0
}

[ -f "$MARKER" ] && exit 0

SOC_DIR_NAME=$(resolve_soc) || exit 0
SQLITE3_BIN=$(resolve_sqlite3) || exit 0
DATABASE_PATH=$(resolve_database) || exit 0
JSON_DIR="$JSON_ROOT/$SOC_DIR_NAME"

success_count=0
found_count=0

[ -d "$JSON_DIR" ] && apply_json_dir "$JSON_DIR"

[ "$found_count" -gt 0 ] && [ "$success_count" -gt 0 ] && touch "$MARKER"
exit 0
