#!/bin/bash
set -e  # Exit on any error

# Colors for output
RED='\e[0;31m'
GREEN='\e[0;32m'
YELLOW='\e[1;33m'
BLUE='\e[0;34m'
NC='\e[0m' # No Color

# Configuration
AUR_DIR="aur"
# The AUR repo's local branch and the remote/branch it is published to
# (the AUR only accepts pushes to master). Set AUR_PUSH=0 to commit the AUR
# update without pushing it.
AUR_REMOTE="${AUR_REMOTE:-aur}"
AUR_LOCAL_BRANCH="${AUR_LOCAL_BRANCH:-main}"
AUR_REMOTE_BRANCH="${AUR_REMOTE_BRANCH:-master}"
AUR_PUSH="${AUR_PUSH:-1}"
MAIN_BRANCH="master"
ANNOTATION=""
FORCE_UPDATE=0

# Helper functions
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  MX Live USB Maker Arch Release Script${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo
}

print_step() {
    echo -e "${GREEN}> $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}WARNING: $1${NC}"
}

print_error() {
    echo -e "${RED}ERROR: $1${NC}"
}

print_success() {
    echo -e "${GREEN}OK: $1${NC}"
}

# Validate version format (semantic versioning or YY.MM format)
validate_version() {
    local version=$1

    # Remove 'v' prefix if present for validation
    local clean_version=${version#v}

    # Check semantic version format (major.minor.patch), YY.MM, or YY.MMsuffix format
    if ! [[ $clean_version =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?[a-z]*$ ]]; then
        print_error "Invalid version format: $version"
        echo "Expected formats: 1.0.0, v1.0.0, YY.MM (like 26.01), or YY.MMsuffix (like 26.01arch)"
        exit 1
    fi

    echo "$version"
}

# Check if tag already exists
check_tag_exists() {
    local version=$1

    # Check local tags
    if git tag -l | grep -q "^${version}$"; then
        print_error "Tag '$version' already exists locally"
        return 1
    fi

    # Check remote tags
    if git ls-remote --tags mxlinux 2>/dev/null | grep -q "refs/tags/${version}$"; then
        print_error "Tag '$version' already exists on remote"
        return 1
    fi

    return 0
}

# Get latest tag version for comparison
get_latest_tag() {
    # Filter to only version-like tags (v?X.Y or v?X.Y.Z with optional suffix)
    local latest_tag=$(git tag -l | grep -E '^v?[0-9]+\.[0-9]+(\.[0-9]+)?[a-z]*$' | sort -V | tail -n1)
    if [ -z "$latest_tag" ]; then
        echo "0.0.0"  # No tags exist yet
    else
        echo "${latest_tag#v}"  # Remove 'v' prefix for comparison
    fi
}

# Compare versions (returns 0 if new > old, 1 if new <= old)
compare_versions() {
    local new_version=$1
    local old_version=$2

    # Remove 'v' prefix for comparison
    new_version=${new_version#v}
    old_version=${old_version#v}

    # Use sort -V for semantic version comparison
    if [ "$new_version" = "$(echo -e "$new_version\n$old_version" | sort -V | tail -n1)" ] && [ "$new_version" != "$old_version" ]; then
        return 0  # new > old
    else
        return 1  # new <= old
    fi
}

# Prompt user for annotation
prompt_annotation() {
    local version=$1

    echo
    print_step "Enter release annotation/notes"

    local annotation=""
    local tmpfile
    tmpfile=$(mktemp) || {
        print_error "Failed to create temp file"
        exit 1
    }
    cat > "$tmpfile" <<EOF
## Release $version

- Feature 1
- Bug fix 2
- Other changes
EOF

    if [ -n "${EDITOR:-}" ]; then
        "${EDITOR}" "$tmpfile"
    else
        echo "No EDITOR set. Enter a single-line annotation and press Enter:"
        echo "(Multi-line notes require setting EDITOR.)"
        read -r annotation
        if [ -n "$annotation" ]; then
            printf "%s\n" "$annotation" > "$tmpfile"
        fi
    fi

    annotation=$(sed '/^[[:space:]]*$/d' "$tmpfile")
    rm -f "$tmpfile"

    if [ -z "$annotation" ]; then
        print_error "Annotation cannot be empty"
        exit 1
    fi

    ANNOTATION="$annotation"
}

# Create annotated tag
create_tag() {
    local version=$1
    local annotation=$2

    print_step "Creating annotated tag '$version'..."

    # Create annotated tag
    git tag -a "$version" -m "$annotation"

    print_success "Tag '$version' created successfully"
    echo
    git show "$version" --stat
}

# Update AUR package
update_aur_package() {
    local version=$1
    local annotation=$2

    print_step "Updating AUR package..."

    # Check if aur directory exists
    if [ ! -d "$AUR_DIR" ]; then
        print_error "AUR directory '$AUR_DIR' not found"
        exit 1
    fi

    local pkgbuild="$AUR_DIR/PKGBUILD"
    local srcinfo="$AUR_DIR/.SRCINFO"

    # Update PKGBUILD pkgver to match tag and remove pkgver() if present
    if [ -f "$pkgbuild" ]; then
        print_step "Updating PKGBUILD pkgver to $version..."
        if rg -q "^pkgver=" "$pkgbuild"; then
            sed -i "s/^pkgver=.*/pkgver=${version}/" "$pkgbuild"
        else
            awk -v ver="$version" '
                /^pkgname=/ { print; print "pkgver=" ver; next }
                { print }
            ' "$pkgbuild" > "$pkgbuild.tmp" && mv "$pkgbuild.tmp" "$pkgbuild"
        fi

        if rg -q "^pkgver\\(\\)" "$pkgbuild"; then
            awk '
                BEGIN { in_pkgver = 0 }
                /^pkgver\\(\\)/ { in_pkgver = 1; next }
                in_pkgver && /^}/ { in_pkgver = 0; next }
                !in_pkgver { print }
            ' "$pkgbuild" > "$pkgbuild.tmp" && mv "$pkgbuild.tmp" "$pkgbuild"
        fi
    else
        print_error "PKGBUILD not found in $AUR_DIR"
        exit 1
    fi

    # Convert to tarball source and calculate checksum
    print_step "Converting to tarball source and calculating checksum..."
    local tarball_url="https://github.com/MX-Linux/mx-live-usb-maker/archive/refs/tags/${version}.tar.gz"

    # Update source in PKGBUILD
    sed -i "s|source=.*|source=(\"${tarball_url}\")|" "$pkgbuild"

    # Remove git from makedepends if present
    sed -i "s/'git' //;s/ 'git'//" "$pkgbuild"

    # Download tarball and calculate checksum (with retry)
    local checksum=""
    local retries=5
    for i in $(seq 1 $retries); do
        print_step "Attempting to download tarball (attempt $i/$retries)..."
        if curl -L --fail --silent --show-error "$tarball_url" -o "/tmp/${version}.tar.gz" 2>/dev/null; then
            checksum=$(sha256sum "/tmp/${version}.tar.gz" | cut -d' ' -f1)
            if [ -n "$checksum" ]; then
                print_success "Checksum calculated: ${checksum:0:16}..."
                break
            fi
        fi

        if [ $i -lt $retries ]; then
            print_warning "Failed to download tarball, waiting 5 seconds before retry..."
            sleep 5
        fi
    done

    if [ -z "$checksum" ]; then
        print_error "Failed to download tarball after $retries attempts"
        print_warning "Nothing was committed; $AUR_DIR/PKGBUILD has uncommitted edits. Re-run once the tag tarball is reachable."
        exit 1
    fi

    # Update checksum in PKGBUILD
    sed -i "s/sha256sums=.*/sha256sums=('${checksum}')/" "$pkgbuild"

    # Regenerate .SRCINFO from PKGBUILD
    print_step "Regenerating .SRCINFO..."
    (cd "$AUR_DIR" && makepkg --printsrcinfo) > "$srcinfo"
    # Sanity-check what is about to be committed (and pushed)
    if ! awk -v v="${version}" '$1 == "pkgver" && $3 == v { f = 1 } END { exit !f }' "$srcinfo"; then
        print_error ".SRCINFO does not report pkgver = ${version}"
        exit 1
    fi
    if ! awk -v c="$checksum" '$1 == "sha256sums" && $3 == c { f = 1 } END { exit !f }' "$srcinfo"; then
        print_error ".SRCINFO does not carry the downloaded tarball checksum"
        exit 1
    fi

    # Check if there are any changes to commit
    print_step "Checking for AUR package changes..."

    if git -C "$AUR_DIR" diff --quiet && git -C "$AUR_DIR" diff --staged --quiet; then
        print_warning "No changes in AUR package - skipping commit"
    else
        print_step "Committing AUR package changes..."

        # Add all changes
        git -C "$AUR_DIR" add .

        # Commit changes
        git -C "$AUR_DIR" commit -m "$annotation"

        print_success "AUR package updated and committed"
        echo
        git -C "$AUR_DIR" show --stat HEAD
    fi

    # Clean up downloaded tarball
    rm -f "/tmp/${version}.tar.gz"
}

# Push the AUR repo if it has commits the AUR doesn't
push_aur_package() {
    print_step "Pushing AUR package..."

    local aur_toplevel
    aur_toplevel=$(git -C "$AUR_DIR" rev-parse --show-toplevel 2>/dev/null) || aur_toplevel=""
    if [ "$aur_toplevel" != "$(realpath "$AUR_DIR")" ]; then
        print_error "$AUR_DIR is not a separate git checkout"
        return 1
    fi

    if ! git -C "$AUR_DIR" diff --quiet HEAD -- PKGBUILD .SRCINFO; then
        print_error "$AUR_DIR has uncommitted PKGBUILD/.SRCINFO changes; commit them first"
        return 1
    fi

    local current_branch
    current_branch=$(git -C "$AUR_DIR" branch --show-current)
    if [ "$current_branch" != "$AUR_LOCAL_BRANCH" ]; then
        print_error "$AUR_DIR is on branch '$current_branch', expected '$AUR_LOCAL_BRANCH'"
        return 1
    fi

    if ! git -C "$AUR_DIR" fetch --quiet "$AUR_REMOTE" "$AUR_REMOTE_BRANCH"; then
        print_error "Could not fetch $AUR_REMOTE/$AUR_REMOTE_BRANCH"
        return 1
    fi
    local ahead
    ahead=$(git -C "$AUR_DIR" rev-list --count "$AUR_REMOTE/$AUR_REMOTE_BRANCH..$AUR_LOCAL_BRANCH")
    if [ "$ahead" -eq 0 ]; then
        print_success "AUR already up to date; nothing to push"
        return 0
    fi

    if ! git -C "$AUR_DIR" push "$AUR_REMOTE" "$AUR_LOCAL_BRANCH:$AUR_REMOTE_BRANCH"; then
        print_error "Pushing to $AUR_REMOTE failed"
        return 1
    fi
    print_success "Pushed $ahead commit(s) to $AUR_REMOTE/$AUR_REMOTE_BRANCH"
}

# Push to the AUR, or print the manual steps when pushing is disabled or fails
publish_aur_package() {
    if [ "$AUR_PUSH" = 1 ] && push_aur_package; then
        return 0
    fi
    show_push_instructions "$@"
    # A failed push is an error; a disabled one is not
    [ "$AUR_PUSH" != 1 ]
}

# Show manual push instructions
show_push_instructions() {
    local version=$1

    echo
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  MANUAL PUSH REQUIRED${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo
    print_warning "Please run this command manually:"
    echo
    echo "# Push AUR package update:"
    echo -e "${YELLOW}git -C $AUR_DIR push $AUR_REMOTE $AUR_LOCAL_BRANCH:$AUR_REMOTE_BRANCH${NC}"
    echo
    print_step "The tag has been created and pushed automatically."
    print_step "After pushing the AUR changes, the package will be ready for AUR submission."
}

# Main script
main() {
    local version=""
    local mode="release"

    # Parse arguments
    if [ $# -eq 0 ]; then
        print_error "Usage: $0 <version> [--update|--force]"
        echo "Example: $0 1.0.0 or $0 v1.0.0"
        echo "         $0 26.01 --update"
        exit 1
    fi

    version=$1
    if [ $# -gt 1 ]; then
        case "$2" in
            --update|--force)
                mode="update"
                FORCE_UPDATE=1
                ;;
            *)
                print_error "Unknown option: $2"
                exit 1
                ;;
        esac
    fi

    print_header

    # Validate and normalize version
    print_step "Validating version format..."
    version=$(validate_version "$version")
    print_success "Version format valid: $version"

    if [ "$mode" = "update" ]; then
        print_step "Update-only mode enabled: skipping tag checks and creation"
        update_aur_package "$version" "Update AUR package to $version"
        publish_aur_package "$version"
        print_success "AUR update complete (no tag created)"
        return 0
    fi

    # Check if we're in a git repository
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        print_error "Not in a git repository"
        exit 1
    fi

    # Check if on main branch
    local current_branch=$(git branch --show-current)
    if [ "$current_branch" != "$MAIN_BRANCH" ]; then
        print_warning "Not on $MAIN_BRANCH branch (currently on: $current_branch)"
        echo "Continue anyway? (y/N)"
        read -r response
        if [[ ! "$response" =~ ^[Yy]$ ]]; then
            print_step "Aborted by user"
            exit 0
        fi
    fi

    # Check if tag exists
    print_step "Checking if tag '$version' already exists..."
    if ! check_tag_exists "$version"; then
        exit 1
    fi
    print_success "Tag '$version' is available"

    # Compare with latest tag
    print_step "Checking version progression..."
    local latest_tag=$(get_latest_tag)
    local clean_version=${version#v}

    if ! compare_versions "$clean_version" "$latest_tag"; then
        print_error "Version $clean_version is not higher than latest tag $latest_tag"
        exit 1
    fi
    print_success "Version $version > $latest_tag"

    # Prompt for annotation
    prompt_annotation "$version"
    local annotation="$ANNOTATION"

    # Confirm before proceeding
    echo
    print_warning "About to create tag '$version' with annotation:"
    echo "$annotation"
    echo
    echo "Continue? (y/N)"
    read -r response
    if [[ ! "$response" =~ ^[Yy]$ ]]; then
        print_step "Aborted by user"
        exit 0
    fi

    # Create the tag
    create_tag "$version" "$annotation"

    # Push the tag immediately (needed for checksum calculation)
    print_step "Pushing tag to GitHub..."
    git push mxlinux "$version"
    print_success "Tag pushed to GitHub"

    # Update AUR package (now with real checksum)
    update_aur_package "$version" "$annotation"

    # Show manual push instructions (only AUR now)
    publish_aur_package "$version"

    echo
    print_success "Release preparation complete!"
}

# Run main function with all arguments
main "$@"
